/**************************************************************************
 *
 * FMR2 - FeliCa MRT RapidPass Reader
 * Project URL: https://github.com/nazdridoy/fmr2
 *
 * An Arduino sketch for robustly reading the balance and transaction
 * history from a Dhaka MRT RapidPass card using a PN532 NFC module.
 *
 * Author: nazDridody (https://github.com/nazdridoy)
 * License: AGPLv3
 *
 * For detailed information, features, hardware requirements, and usage
 * instructions, please refer to the project's README.md file.
 *
 **************************************************************************/
#include <Arduino.h>

// --- Communication Protocol Selection ---
// Use #if 1 to select your board's interface
// and connect the pins accordingly.
#if 0  // <--- Set to 1 for SPI
  #include <SPI.h>
  #include <PN532_SPI.h>
  #include <PN532.h>

  // The SS pin can be changed here.
  // Common pins: Arduino Uno/Nano: 10, ESP8266: 15 (D8), ESP32: 5
  const int PN532_SS = 10;
  PN532_SPI pn532spi(SPI, PN532_SS);
  PN532 nfc(pn532spi);

#elif 0  // <--- Set to 1 for I2C
  #include <Wire.h>
  #include <PN532_I2C.h>
  #include <PN532.h>

  PN532_I2C pn532i2c(Wire);
  PN532 nfc(pn532i2c);

#elif 0  // <--- Set to 1 for HSU (Hardware Serial)
  #include <PN532_HSU.h>
  #include <PN532.h>

  // Use Hardware Serial for HSU.
  // On Mega, use Serial1, Serial2, etc. On Uno/Nano, this conflicts with the monitor.
  PN532_HSU pn532hsu(Serial1);
  PN532 nfc(pn532hsu);

#else  // <--- Default: SWHSU (Software Serial)
  #include <SoftwareSerial.h>
  #include <PN532_SWHSU.h>
  #include <PN532.h>

  // --- Hardware Setup (for Software Serial) ---
  const int PN532_RX = 3; // To PN532's TX
  const int PN532_TX = 2; // To PN532's RX
  SoftwareSerial SWSerial(PN532_RX, PN532_TX);
  PN532_SWHSU pn532swhsu(SWSerial);
  PN532 nfc(pn532swhsu);
#endif

#include <PN532_debug.h>

// --- Configurable Constants ---
#define ENABLE_RAW_BLOCK_DUMP true // Set to false to hide raw hex data
const uint16_t FELICA_SYSTEM_CODE = 0x90E3;
const uint16_t TRANSACTION_SERVICE_CODE = 0x220F;
const uint8_t NUM_TRANSACTION_BLOCKS = 20;
const uint8_t MAX_READ_RETRIES = 3;
const uint8_t READ_RETRY_DELAY_MS = 5;

// --- Global variables for state management ---
uint8_t       _prevIDm[8];
unsigned long _prevTime;
int8_t        g_scMode = -1; // Cached FeliCa Read Mode
int8_t        g_blMode = -1; // Cached FeliCa Block Mode

// --- Data Structure for a Transaction ---
enum TransactionKind { UNKNOWN, COMMUTE, BALANCE_UPDATE };

struct Transaction {
  bool isValid;
  uint8_t blockIndex;
  uint32_t timestampValue;
  uint8_t fromStationCode;
  uint8_t toStationCode;
  uint32_t balance;
  long amount;
  TransactionKind kind;
};

// --- Forward Declarations ---
static bool readAllTransactionBlocks(uint8_t rawBlocks[][16]);
static bool readSingleBlockWithRetry(uint8_t blockNo, uint8_t blockData[16]);
static bool readRapidPassBlocks(uint8_t startBlock, uint8_t numBlocks, uint8_t blockData[][16]);
static void printStationName(uint8_t code);
static void parseAllBlocks(const uint8_t rawBlocks[][16], Transaction transactions[]);
static void processTransactions(Transaction transactions[]);
static void printProcessedTransactions(const Transaction transactions[]);
void printPaddedString(const char* text, uint8_t width);
void printPaddedNumber(long number, uint8_t width, bool showPlus);


/**************************************************************************/
/*
    SETUP: Initializes Serial, NFC, and prints board info.
*/
/**************************************************************************/
void setup(void) {
  Serial.begin(115200);
  while (!Serial) { delay(10); } // Wait for serial port to connect.

  Serial.println(F("Robust FeliCa Card Reader"));
  
  // Add a delay to allow the PN532 to boot up before initialization.
  // This helps prevent a race condition on power-up.
  
  delay(400);

  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.print(F("Didn't find PN53x board"));
    while (1) { delay(10); } // halt
  }

  Serial.print(F("Found chip PN5")); Serial.println((versiondata >> 24) & 0xFF, HEX);
  Serial.print(F("Firmware ver. ")); Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.'); Serial.println((versiondata >> 8) & 0xFF, DEC);

  nfc.setPassiveActivationRetries(0xFF);
  nfc.SAMConfig();

  memset(_prevIDm, 0, 8);
  Serial.println(F("Waiting for a FeliCa card..."));
}


/**************************************************************************/
/*
    LOOP: Main execution loop.
*/
/**************************************************************************/
void loop(void) {
  uint8_t idm[8];
  uint8_t pmm[8];
  uint16_t systemCodeResponse;

  // 1. Poll for a new card
  if (nfc.felica_Polling(FELICA_SYSTEM_CODE, 0x00, idm, pmm, &systemCodeResponse, 5000) != 1) {
    return; // No card found, loop again
  }

  // Debounce for the same card
  if (memcmp(idm, _prevIDm, 8) == 0 && (millis() - _prevTime) < 3000) {
    return;
  }

  // --- New Card Detected ---
  memcpy(_prevIDm, idm, 8);
  _prevTime = millis();

  Serial.println(F("---------------------------------"));
  Serial.println(F("Found a new card!"));
  Serial.print(F("  IDm: ")); nfc.PrintHex(idm, 8);
  Serial.print(F("  PMm: ")); nfc.PrintHex(pmm, 8);
  Serial.print(F("  System Code: ")); Serial.println(systemCodeResponse, HEX);

  // 2. Read all transaction blocks into a buffer using a robust strategy
  uint8_t rawBlocks[NUM_TRANSACTION_BLOCKS][16];
  Serial.println(F("Reading all transaction blocks..."));
  if (!readAllTransactionBlocks(rawBlocks)) {
    Serial.println(F("Failed to read card data."));
    Serial.println(F("---------------------------------"));
    return;
  }
  Serial.println(F("Read complete."));

  // 3. (Optional) Print raw data dump
  if (ENABLE_RAW_BLOCK_DUMP) {
    Serial.println(F("--- Raw Block Data (0-19) ---"));
    for (uint8_t i = 0; i < NUM_TRANSACTION_BLOCKS; i++) {
      Serial.print(F("Block ")); Serial.print(i); Serial.print(F(": "));
      for (int j = 0; j < 16; j++) {
        if (rawBlocks[i][j] < 0x10) Serial.print('0');
        Serial.print(rawBlocks[i][j], HEX);
        Serial.print(' ');
      }
      Serial.println();
    }
  }

  // 4. Parse raw blocks into Transaction structs
  Transaction transactions[NUM_TRANSACTION_BLOCKS];
  parseAllBlocks(rawBlocks, transactions);

  // 5. Process transactions to calculate amount and kind
  processTransactions(transactions);

  // 6. Print the final, processed report
  printProcessedTransactions(transactions);
  
  Serial.println(F("---------------------------------"));

  Serial.println(F("Card access completed."));
  Serial.println(F("Card read complete. Same card will be ignored for 3 seconds."));
  Serial.println(F("---------------------------------"));
  Serial.println(F("Waiting for a FeliCa card..."));
  delay(1000);
}


/**************************************************************************/
/*
    Reads all transaction blocks using a block-by-block strategy.
*/
/**************************************************************************/
bool readAllTransactionBlocks(uint8_t rawBlocks[][16]) {
  memset(rawBlocks, 0, sizeof(uint8_t) * NUM_TRANSACTION_BLOCKS * 16);

  // Read one-by-one with retries. This is slower but more reliable.
  uint8_t blocksRead = 0;
  for (uint8_t i = 0; i < NUM_TRANSACTION_BLOCKS; i++) {
    if (readSingleBlockWithRetry(i, rawBlocks[i])) {
      blocksRead++;
    } else {
      Serial.print(F("Warning: Failed to read block "));
      Serial.println(i);
      // Block is already zeroed, so parsing will skip it.
    }
  }

  return blocksRead > 0;
}

/**************************************************************************/
/*
    Reads a single block, retrying on failure.
*/
/**************************************************************************/
static bool readSingleBlockWithRetry(uint8_t blockNo, uint8_t blockData[16]) {
  uint8_t buffer[1][16];
  for (uint8_t i = 0; i < MAX_READ_RETRIES; i++) {
    if (readRapidPassBlocks(blockNo, 1, buffer)) {
      memcpy(blockData, buffer[0], 16);
      return true;
    }
    delay(READ_RETRY_DELAY_MS);
  }
  return false;
}

/**************************************************************************/
/*
    Parses all raw blocks from a buffer into an array of Transaction structs.
*/
/**************************************************************************/
void parseAllBlocks(const uint8_t rawBlocks[][16], Transaction transactions[]) {
  for (uint8_t i = 0; i < NUM_TRANSACTION_BLOCKS; i++) {
    transactions[i].blockIndex = i;

    // Check if block is empty (all 0x00 or all 0xFF)
    bool allZero = true, allFF = true;
    for (int j = 0; j < 16; j++) {
      if (rawBlocks[i][j] != 0x00) allZero = false;
      if (rawBlocks[i][j] != 0xFF) allFF = false;
    }
    if (allZero || allFF) {
      transactions[i].isValid = false;
      continue;
    }

    transactions[i].isValid = true;
    transactions[i].timestampValue = ((uint32_t)rawBlocks[i][4] << 16) | ((uint32_t)rawBlocks[i][5] << 8) | (uint32_t)rawBlocks[i][6];
    transactions[i].fromStationCode = rawBlocks[i][8];
    transactions[i].toStationCode = rawBlocks[i][10];
    transactions[i].balance = (uint32_t)rawBlocks[i][11] | ((uint32_t)rawBlocks[i][12] << 8) | ((uint32_t)rawBlocks[i][13] << 16);
  }
}

/**************************************************************************/
/*
    Processes an array of transactions to determine amount and kind.
*/
/**************************************************************************/
void processTransactions(Transaction transactions[]) {
  for (uint8_t i = 0; i < NUM_TRANSACTION_BLOCKS; i++) {
    if (!transactions[i].isValid) continue;

    // Find the next valid transaction (older in time)
    const Transaction* nextValidTx = nullptr;
    for (uint8_t j = i + 1; j < NUM_TRANSACTION_BLOCKS; j++) {
      if (transactions[j].isValid) {
        nextValidTx = &transactions[j];
        break;
      }
    }

    // Calculate amount
    if (nextValidTx) {
      transactions[i].amount = (long)transactions[i].balance - (long)nextValidTx->balance;
    } else {
      transactions[i].amount = 0; // Cannot determine amount for the oldest transaction
    }

    // Determine kind
    if (transactions[i].toStationCode == 0 || transactions[i].fromStationCode == 0) {
      transactions[i].kind = BALANCE_UPDATE;
    } else if (transactions[i].amount > 0) {
      transactions[i].kind = BALANCE_UPDATE;
    } else {
      transactions[i].kind = COMMUTE;
    }
  }
}

/***************************************************************************/
/*
    Helper function to print a string and pad it with spaces for alignment.
*/
/***************************************************************************/
void printPaddedString(const char* text, uint8_t width) {
    int len = strlen(text);
    Serial.print(text);
    for (int i = len; i < width; i++) {
        Serial.print(' ');
    }
}

/***************************************************************************/
/*
    Helper function to print a number right-aligned with padding.
*/
/***************************************************************************/
void printPaddedNumber(long number, uint8_t width, bool showPlus) {
    char buf[15];
    int len;

    if (showPlus && number > 0) {
        len = snprintf(buf, sizeof(buf), "+%ld", number);
    } else {
        len = snprintf(buf, sizeof(buf), "%ld", number);
    }

    // Print padding spaces before the number for right-alignment
    for (int i = len; i < width; i++) {
        Serial.print(' ');
    }
    Serial.print(buf);
}

/***************************************************************************/
/*
    Prints a clean, formatted report of all processed transactions.
*/
/***************************************************************************/
void printProcessedTransactions(const Transaction transactions[]) {
  Serial.println(F("--- Processed Transaction Report ---"));
  if (transactions[0].isValid) {
    Serial.print(F("Latest Balance: "));
    Serial.print(transactions[0].balance);
    Serial.println(F(" BDT"));
  } else {
    Serial.println(F("Could not determine balance. Card may be new or empty."));
  }
  Serial.println(F("------------------------------------"));

  for (uint8_t i = 0; i < NUM_TRANSACTION_BLOCKS; i++) {
    if (!transactions[i].isValid) continue;

    const Transaction* tx = &transactions[i];

    // Print Tx number, left-aligned
    char txBuf[8];
    snprintf(txBuf, sizeof(txBuf), "Tx %d:", tx->blockIndex);
    printPaddedString(txBuf, 8);

    // Print timestamp (already fixed width)
    Serial.print(F("["));
    uint8_t hour  = (uint8_t)((tx->timestampValue >> 3)  & 0x1F);
    uint8_t day   = (uint8_t)((tx->timestampValue >> 8)  & 0x1F);
    uint8_t month = (uint8_t)((tx->timestampValue >> 13) & 0x0F);
    uint8_t year5 = (uint8_t)((tx->timestampValue >> 17) & 0x1F);
    Serial.print(2000 + year5); Serial.print('-');
    if (month < 10) Serial.print('0'); Serial.print(month); Serial.print('-');
    if (day < 10) Serial.print('0'); Serial.print(day); Serial.print(' ');
    if (hour < 10) Serial.print('0'); Serial.print(hour % 24); Serial.print(F(":00] "));

    // Print Kind, left-aligned
    switch (tx->kind) {
      case COMMUTE:        printPaddedString("COMMUTE", 15); break;
      case BALANCE_UPDATE: printPaddedString("BALANCE_UPDATE", 15); break;
      default:             printPaddedString("UNKNOWN", 15); break;
    }

    // Print Amount, right-aligned
    Serial.print(F("| Amount: "));
    printPaddedNumber(tx->amount, 6, true);

    // Print Balance, right-aligned
    Serial.print(F(" | Balance: "));
    printPaddedNumber(tx->balance, 6, false);

    // Print Stations
    if (tx->kind == COMMUTE) {
      Serial.print(F(" | From: ")); printStationName(tx->fromStationCode);
      Serial.print(F(" To: ")); printStationName(tx->toStationCode);
    } else if (tx->kind == BALANCE_UPDATE) {
      // For top-ups, one of the station codes usually indicates the location.
      if (tx->fromStationCode != 0) {
        Serial.print(F(" | At: ")); printStationName(tx->fromStationCode);
      } else if (tx->toStationCode != 0) {
        Serial.print(F(" | At: ")); printStationName(tx->toStationCode);
      }
    }
    Serial.println();
  }
}


// --- Low-level Helper Functions ---

/**************************************************************************/
/*
    Performs the low-level FeliCa Read Without Encryption command.
    Tries to use a cached mode for speed, otherwise discovers a working mode.
*/
/**************************************************************************/
static bool readRapidPassBlocks(uint8_t startBlock, uint8_t numBlocks, uint8_t blockData[][16]) {
  if (numBlocks == 0 || numBlocks > 20) return false;

  const uint16_t scA = TRANSACTION_SERVICE_CODE;
  const uint16_t scB = ((TRANSACTION_SERVICE_CODE & 0xFF) << 8) | (TRANSACTION_SERVICE_CODE >> 8);

  uint16_t blockListA[numBlocks];
  uint16_t blockListB[numBlocks];
  for (uint8_t i = 0; i < numBlocks; i++) {
    uint8_t bno = startBlock + i;
    blockListA[i] = (uint16_t)(0x8000 | bno);
    blockListB[i] = (uint16_t)((bno << 8) | 0x80);
  }

  if (g_scMode != -1 && g_blMode != -1) {
    const uint16_t* scPtr = (g_scMode == 0) ? &scA : &scB;
    const uint16_t* blPtr = (g_blMode == 0) ? blockListA : blockListB;
    if (nfc.felica_ReadWithoutEncryption(1, scPtr, numBlocks, blPtr, blockData) >= 0) {
      return true;
    }
    g_scMode = -1; g_blMode = -1; // Reset cache on failure
  }

  // Discover a working combination
  const uint16_t* scs[] = {&scA, &scB};
  const uint16_t* bls[] = {blockListA, blockListB};
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < 2; j++) {
      if (nfc.felica_ReadWithoutEncryption(1, scs[i], numBlocks, bls[j], blockData) >= 0) {
        g_scMode = i; g_blMode = j;
        return true;
      }
    }
  }
  return false;
}

/**************************************************************************/
/*
    Maps a station code to its name for printing.
*/
/**************************************************************************/
static void printStationName(uint8_t code) {
  // Map from mrt-buddy StationService
  switch (code) {
    case 10:  Serial.print(F("Motijheel")); break;
    case 20:  Serial.print(F("Bangladesh Secretariat")); break;
    case 25:  Serial.print(F("Dhaka University")); break;
    case 30:  Serial.print(F("Shahbagh")); break;
    case 35:  Serial.print(F("Karwan Bazar")); break;
    case 40:  Serial.print(F("Farmgate")); break;
    case 45:  Serial.print(F("Bijoy Sarani")); break;
    case 50:  Serial.print(F("Agargaon")); break;
    case 55:  Serial.print(F("Shewrapara")); break;
    case 60:  Serial.print(F("Kazipara")); break;
    case 65:  Serial.print(F("Mirpur 10")); break;
    case 70:  Serial.print(F("Mirpur 11")); break;
    case 75:  Serial.print(F("Pallabi")); break;
    case 80:  Serial.print(F("Uttara South")); break;
    case 85:  Serial.print(F("Uttara Center")); break;
    case 90:  Serial.print(F("Uttara North")); break;
    default:
      Serial.print(F("Unknown Station ("));
      Serial.print(code);
      Serial.print(')');
      break;
  }
}
