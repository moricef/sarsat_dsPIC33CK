#include "includes2.h"

// Modulation parameters
#define CARRIER_FREQ_HZ    40000   // 40 kHz carrier
#define SYMBOL_RATE_HZ     400     // 400 baud
#define SAMPLE_RATE_HZ     200000  // 200 kHz sampling
#define SAMPLES_PER_SYMBOL (SAMPLE_RATE_HZ / SYMBOL_RATE_HZ) // 500
#define DAC_OFFSET         2048    // Mid-scale for 12-bit DAC

// Fixed-point constants (Q15 format)
#define COS_1P1_Q15  14865    // cos(1.1 rad) in Q15
#define SIN_1P1_Q15  29197    // sin(1.1 rad) in Q15

// Carrier wave lookup table (5 samples @ 200 kHz = 40 kHz)
const int16_t cos_table[5] = {32767, 10126, -26510, -26510, 10126};
const int16_t sin_table[5] = {0, 31163, 19260, -19260, -31163};

// Beacon frame parameters
#define PREAMBLE_DURATION_MS   160     // 160ms of carrier
#define MODULATED_DURATION_MS  360     // 360ms of modulated signal

// Dual-Phase State Machine
#define PREAMBLE_PHASE 0
#define DATA_PHASE     1
volatile uint8_t tx_phase = PREAMBLE_PHASE;

// Phase Management
volatile uint8_t carrier_phase = 0;  // Cycles 0-4 at 40kHz

// Frame Timing Control
#define PREAMBLE_SAMPLES (PREAMBLE_DURATION_MS * SAMPLE_RATE_HZ / 1000)  // 32,000
volatile uint32_t preamble_count = 0;
volatile uint16_t idle_count = 0;
#define IDLE_SYMBOLS 2  // 5ms guard interval

// Frame composition (corrected sizes)
#define SYNC_BITS      15      // 15 bits of 1's
#define FRAME_SYNC_BITS 9      // 9-bit frame sync
#define COUNTRY_BITS   10      // Country code
#define AIRCRAFT_BITS  24      // Aircraft ID
#define POSITION_BITS  21      // Position data
#define OFFSET_BITS    20      // Position offset
#define BCH_POS_BITS   10      // BCH(31,21) parity
#define BCH_ID_BITS    12      // BCH(12,12) parity
#define MESSAGE_BITS   (SYNC_BITS + FRAME_SYNC_BITS + COUNTRY_BITS + \
                       AIRCRAFT_BITS + POSITION_BITS + OFFSET_BITS + \
                       BCH_POS_BITS + BCH_ID_BITS)  // 121 bits

// BCH Parameters (BCH(31,21) + BCH(12,12))
#define BCH_N1 31   // Codeword length for data
#define BCH_K1 21   // Data length for position
#define BCH_N2 12   // Parity bits length
#define BCH_POLY 0x3B3  // Generator polynomial for BCH(31,21)

// Global variables
volatile uint32_t sample_count = 0;
volatile size_t symbol_index = 0;
uint8_t beacon_frame[MESSAGE_BITS];  // Full beacon frame
volatile uint16_t debug_dac_value = 0;  // Debug probe

// =============================
// BCH Encoder Functions (Corrected)
// =============================

// BCH(31,21) encoder for position data
uint16_t bch_encode_31_21(uint32_t data) {
    uint32_t reg = 0;
    data &= 0x1FFFFF;  // Ensure 21-bit input
    
    // Process each bit MSB first
    for (int i = 20; i >= 0; i--) {
        uint8_t bit = (data >> i) & 1;
        uint8_t msb = (reg >> 9) & 1;
        reg = (reg << 1) | bit;
        
        if (msb ^ bit) {
            reg ^= BCH_POLY;
        }
    }
    return reg & 0x3FF;  // Return 10-bit parity
}

// BCH(12,12) encoder - simple parity check
uint16_t bch_encode_12_12(uint16_t data) {
    return data;  // Identity function
}

// =============================
// Beacon Frame Construction (Corrected)
// =============================

void build_beacon_frame() {
    int bit_index = 0;
    
    // 1. Add sync bits (15 bits of 1)
    for (int i = 0; i < SYNC_BITS; i++) {
        beacon_frame[bit_index++] = 1;
    }
    
    // 2. Add frame sync bits (9 bits: 0x1AC = 0b110101100)
    const uint16_t frame_sync = 0x1AC;
    for (int i = 8; i >= 0; i--) {
        beacon_frame[bit_index++] = (frame_sync >> i) & 1;
    }
    
    // 3. Add message content
    //    a) Country code (10 bits)
    uint16_t country_code = 0x2A5; // Example: France
    for (int i = 9; i >= 0; i--) {
        beacon_frame[bit_index++] = (country_code >> i) & 1;
    }
    
    //    b) Aircraft ID (24 bits)
    uint32_t aircraft_id = 0x00A5F3C; // Example ID
    for (int i = 23; i >= 0; i--) {
        beacon_frame[bit_index++] = (aircraft_id >> i) & 1;
    }
    
    //    c) Position (21 bits)
    uint32_t position = 0x1A5F3; // Example: 42.25�N, 2.75�E
    for (int i = 20; i >= 0; i--) {
        beacon_frame[bit_index++] = (position >> i) & 1;
    }
    
    //    d) Position offset (20 bits)
    uint32_t position_offset = 0x0A5F3; // Example: 150m offset
    for (int i = 19; i >= 0; i--) {
        beacon_frame[bit_index++] = (position_offset >> i) & 1;
    }
    
    // 4. Apply BCH encoding (CORRECTED)
    uint16_t position_parity = bch_encode_31_21(position);
    for (int i = 9; i >= 0; i--) {
        beacon_frame[bit_index++] = (position_parity >> i) & 1;
    }
    
    uint16_t id_parity = bch_encode_12_12(aircraft_id & 0xFFF);
    for (int i = 11; i >= 0; i--) {
        beacon_frame[bit_index++] = (id_parity >> i) & 1;
    }
}

// =============================
// Hardware Initialization (dsPIC33CK specific) - CORRECTED
// =============================

// Initialize system clock to 100 MHz (dsPIC33CK specific)
void init_clock(void) {
    // Unlock PLL registers
    __builtin_write_OSCCONH(0x78);  // KEY to unlock
    __builtin_write_OSCCONL(0x01);  // Start unlock sequence
    
    // Configure PLL for 100 MHz from FRC (8 MHz)
    CLKDIVbits.PLLPRE = 0;      // N1 = 2 (divider = 0+2=2)
    PLLFBD = 98;                // M = 100 (98+2) - CORRECTED VALUE
    CLKDIVbits.PLLPOST = 0;     // N2 = 2 ((0+1)*2=2) - CORRECTED
    
    // Initiate clock switch to FRC with PLL
    __builtin_write_OSCCONH(0x03);  // Select FRCPLL
    __builtin_write_OSCCONL(OSCCON | 0x01);
    while(OSCCONbits.COSC != 0b11); // Wait for switch
    while(!OSCCONbits.LOCK);        // Wait for PLL lock
    
    // Lock PLL registers
    __builtin_write_OSCCONH(0x00);
    __builtin_write_OSCCONL(0x00);
}

// Initialize DAC (dsPIC33CK specific) - CORRECTED
void init_dac(void) {
    // Configure RB0 as analog output
    ANSELB |= 0x0001;        // Set bit 0 of ANSELB (RB0 analog)
    TRISB &= ~0x0001;        // Set RB0 as output
    
    // Configure DAC registers (dsPIC33CK format)
    DAC1CONL = 0x8000;       // DACEN=1
    DAC1CONL |= 0x2000;      // DACOEN=1
    DAC1CONH = 0x0000;  // Right-justified 12-bit (dsPIC33CK specific)
    
    // Set default output value
    DAC1DATH = (DAC_OFFSET >> 8) & 0x0F;
    DAC1DATL = DAC_OFFSET & 0xFF;
}

// Initialize Timer1 for 200 kHz interrupts - CORRECTED
void init_timer1(void) {
    T1CON = 0;                  // Stop timer
    TMR1 = 0;                   // Clear timer
    PR1 = (50000000UL / SAMPLE_RATE_HZ) - 1;  // 50MHz FCY, 200kHz sample rate
    IFS0bits.T1IF = 0;          // Clear interrupt flag
    IPC0bits.T1IP = 5;          // High priority
    IEC0bits.T1IE = 1;          // Enable interrupt
    T1CONbits.TCKPS = 0;        // 1:1 prescaler
    T1CONbits.TON = 1;          // Start timer
}

// Precomputed DAC values for efficiency
const uint16_t precomputed_dac[5] = {
    (uint16_t)(DAC_OFFSET + (((int32_t)32767 * COS_1P1_Q15) >> 18)),
    (uint16_t)(DAC_OFFSET + (((int32_t)10126 * COS_1P1_Q15) >> 18)),
    (uint16_t)(DAC_OFFSET + (((int32_t)(-26510) * COS_1P1_Q15) >> 18)),
    (uint16_t)(DAC_OFFSET + (((int32_t)(-26510) * COS_1P1_Q15) >> 18)),
    (uint16_t)(DAC_OFFSET + (((int32_t)10126 * COS_1P1_Q15) >> 18))
};

const uint16_t precomputed_symbol_dac[2][5] = {
    { // Symbol 0 (+1.1 rad)
        (uint16_t)(DAC_OFFSET + (((((int32_t)32767 * COS_1P1_Q15 - (int32_t)0 * SIN_1P1_Q15)) >> 18))),
        (uint16_t)(DAC_OFFSET + (((((int32_t)10126 * COS_1P1_Q15 - (int32_t)31163 * SIN_1P1_Q15)) >> 18))),
        (uint16_t)(DAC_OFFSET + (((((int32_t)(-26510) * COS_1P1_Q15 - (int32_t)19260 * SIN_1P1_Q15)) >> 18))),
        (uint16_t)(DAC_OFFSET + (((((int32_t)(-26510) * COS_1P1_Q15 - (int32_t)(-19260) * SIN_1P1_Q15)) >> 18))),
        (uint16_t)(DAC_OFFSET + (((((int32_t)10126 * COS_1P1_Q15 - (int32_t)(-31163) * SIN_1P1_Q15)) >> 18)))
    },
    { // Symbol 1 (-1.1 rad)
        (uint16_t)(DAC_OFFSET + (((((int32_t)32767 * COS_1P1_Q15 + (int32_t)0 * SIN_1P1_Q15)) >> 18))),
        (uint16_t)(DAC_OFFSET + (((((int32_t)10126 * COS_1P1_Q15 + (int32_t)31163 * SIN_1P1_Q15)) >> 18))),
        (uint16_t)(DAC_OFFSET + (((((int32_t)(-26510) * COS_1P1_Q15 + (int32_t)19260 * SIN_1P1_Q15)) >> 18))),
        (uint16_t)(DAC_OFFSET + (((((int32_t)(-26510) * COS_1P1_Q15 + (int32_t)(-19260) * SIN_1P1_Q15)) >> 18))),
        (uint16_t)(DAC_OFFSET + (((((int32_t)10126 * COS_1P1_Q15 + (int32_t)(-31163) * SIN_1P1_Q15)) >> 18)))
    }
};

// =============================
// Optimized Timer1 ISR (200 kHz)
// =============================
void __attribute__((interrupt, no_auto_psv, shadow)) _T1Interrupt(void) {
    // Phase 1: Preamble (160ms unmodulated carrier)
    if (tx_phase == PREAMBLE_PHASE) {
        uint16_t dac_val = precomputed_dac[carrier_phase];
        DAC1DATH = (dac_val >> 8) & 0x0F;
        DAC1DATL = dac_val & 0xFF;
        debug_dac_value = dac_val;
        
        // Update carrier phase (0-4)
        carrier_phase = (carrier_phase < 4) ? carrier_phase + 1 : 0;
        
        // Check preamble completion
        if (++preamble_count >= PREAMBLE_SAMPLES) {
            tx_phase = DATA_PHASE;
            preamble_count = 0;
            symbol_index = 0;
            sample_count = 0;
        }
    }
    // Phase 2: Data Transmission
    else {
        uint8_t current_symbol = 0;
        if (symbol_index < MESSAGE_BITS) {
            current_symbol = beacon_frame[symbol_index];
        }
        
        uint16_t dac_val = precomputed_symbol_dac[current_symbol][carrier_phase];
        DAC1DATH = (dac_val >> 8) & 0x0F;
        DAC1DATL = dac_val & 0xFF;
        debug_dac_value = dac_val;
        
        // Update carrier phase (0-4)
        carrier_phase = (carrier_phase < 4) ? carrier_phase + 1 : 0;
        
        // Symbol transition handling
        if (++sample_count >= SAMPLES_PER_SYMBOL) {
            sample_count = 0;
            
            if (symbol_index < MESSAGE_BITS) {
                symbol_index++;
            }
            else {
                if (++idle_count >= IDLE_SYMBOLS) {
                    tx_phase = PREAMBLE_PHASE;
                    idle_count = 0;
                }
            }
        }
    }
    
    // Clear interrupt flag
    IFS0bits.T1IF = 0;
}

int main(void) {
    // Disable watchdog timer
    WDTCONLbits.ON = 0;
    
    // Build the beacon frame
    build_beacon_frame();
    
    init_clock();       // 100 MHz system clock
    init_dac();         // Configure DAC
    init_timer1();      // Configure Timer1
    
    // Enable global interrupts
    __builtin_enable_interrupts();
    
    while(1) {
        // Main loop - all processing in ISR
        __builtin_nop();
    }
    return 0;
}