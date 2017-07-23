#include <Arduino.h>
#include <U8g2lib.h>
#include <Encoder.h>
#include <AD9833.h>
#include <SPI.h>
#include <TimerOne.h>
#include "util/delay.h"
#include <EEPROM.h>

// peripheria
#define DC_RELAY_PIN 6 // high level to short the DC blocking capacitor
#define MCP_CS_PIN 8   // low level to enable the MCP41010

// screen
#define NOKIA_CLK_PIN 17
#define NOKIA_DATA_PIN 16
#define NOKIA_CS_PIN 14
#define NOKIA_DC_PIN 15
#define NOKIA_RESET_PIN 7
U8G2_PCD8544_84X48_1_4W_SW_SPI u8g2( U8G2_R0, NOKIA_CLK_PIN, NOKIA_DATA_PIN, NOKIA_CS_PIN, NOKIA_DC_PIN, NOKIA_RESET_PIN );

// encoder
#define ENC_LEFT_PIN 3
#define ENC_RIGHT_PIN 4
#define ENC_BUTTON_PIN 2
Encoder enc( ENC_LEFT_PIN, ENC_RIGHT_PIN );
long oldEncoderPosition;
long newEncoderPosition;

// generator
#define DDS_FSYNC 9
#define CRYSTAL_CLOCK 25000000UL
#define ACTIVE_REGISTER REG0
AD9833 gen( DDS_FSYNC, CRYSTAL_CLOCK );

// settings
#define FREQ_EDITOR_MAX 12500000UL
#define FREQ_EDITOR_MIN 0UL
#define FREQ_EDITOR_WIDTH 11
#define FREQ_EDITOR_FRACT 2

#define AMP_EDITOR_MAX 100UL
#define AMP_EDITOR_MIN 0UL
#define AMP_EDITOR_WIDTH 5
#define AMP_EDITOR_FRACT 1

#define LONG_PRESS_DURATION 20
#define SHORT_PRESS_DURATION 2

char outputEnabled;
char removeDcOffset;
double frequency;
double amplification;
WaveformType waveform;

typedef enum { NO_EDIT, ENABLED_EDIT, WAVEFORM_EDIT, REMOVEDC_EDIT, FREQ_EDIT, FREQ_SYM_EDIT, AMP_EDIT, AMP_SYM_EDIT, SAVE_EDIT } EditState;
EditState state = NO_EDIT;
uint8_t frequencyEditChar, ampEditChar;
uint8_t scheduleScreenUpdate;

double freqCharMultiplier(uint8_t n) {
    double multiplierLut[ FREQ_EDITOR_WIDTH + 1 ] = { 0, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1, 0, 0.1, 0.01 };
    if (n > FREQ_EDITOR_WIDTH)
        return 0;
    else
        return multiplierLut[n]; 
}

double ampCharMultiplier(uint8_t n) {
    double multiplierLut[ AMP_EDITOR_WIDTH + 1 ] = { 0, 100, 10, 1, 0, 0.1 };
    if ( n > AMP_EDITOR_WIDTH )
        return 0;
    else
        return multiplierLut[n]; 
}

void setup(void) {
    pinMode( ENC_BUTTON_PIN, INPUT);
    attachInterrupt( digitalPinToInterrupt(ENC_BUTTON_PIN), pushed, FALLING );

    Timer1.initialize(250000);
    Timer1.attachInterrupt(timerInterrupt);

    pinMode( DC_RELAY_PIN, OUTPUT );
    digitalWrite( DC_RELAY_PIN, LOW );
  
    pinMode( MCP_CS_PIN, OUTPUT );
    digitalWrite( MCP_CS_PIN, HIGH );

    oldEncoderPosition = 0;

    gen.Begin();
    gen.EnableOutput(false);
    gen.SetOutputSource(ACTIVE_REGISTER);

    u8g2.begin();

    restoreSettings();
}

void loop(void) {
}

void timerInterrupt(void)
{
    if ( state != NO_EDIT ) {
        newEncoderPosition = enc.read();
        if ( newEncoderPosition != oldEncoderPosition ) {
            char clockwise = newEncoderPosition > oldEncoderPosition;
            switch(state) {
                case NO_EDIT:
                    break;
                case ENABLED_EDIT:
                    outputEnabled = !outputEnabled;
                    updateOutput();
                    break;
                case FREQ_EDIT:
                    break;
                case FREQ_SYM_EDIT:
                    frequency = frequency + (clockwise ? 1 : -1) * freqCharMultiplier(frequencyEditChar);
                    updateFrequency();
                    break;
                case WAVEFORM_EDIT:
                    switch(waveform) {
                        case SINE_WAVE:        waveform = clockwise ? TRIANGLE_WAVE    : HALF_SQUARE_WAVE; break;
                        case TRIANGLE_WAVE:    waveform = clockwise ? SQUARE_WAVE      : SINE_WAVE;        break;
                        case SQUARE_WAVE:      waveform = clockwise ? HALF_SQUARE_WAVE : TRIANGLE_WAVE;    break;
                        case HALF_SQUARE_WAVE: waveform = clockwise ? SINE_WAVE        : SQUARE_WAVE;      break;
                        default:               waveform = SINE_WAVE;
                    }
                    updateWaveform();
                    removeDcOffset = ( ( waveform == SQUARE_WAVE ) || ( waveform == HALF_SQUARE_WAVE ) ) ? 0 : 1;
                    updateDcOffsetRemoval();
                    break;
                case REMOVEDC_EDIT:
                    removeDcOffset = !removeDcOffset;
                    updateDcOffsetRemoval();
                    break;
                case AMP_EDIT:
                    break;
                case AMP_SYM_EDIT:
                    amplification = amplification + (clockwise ? 1 : -1) * ampCharMultiplier(ampEditChar);
                    updateAmplification();
                    break;
                case SAVE_EDIT:
                    break;
            }
            oldEncoderPosition = newEncoderPosition;
        }  
    }

    if (scheduleScreenUpdate) {
        redrawScreen();
        scheduleScreenUpdate = 0;
    }
}

void pushed(void) {
    char longPress = 0, shortPress = 0;
    uint8_t counter = 0;
   
    while ( !digitalRead(ENC_BUTTON_PIN) ) {
        if ( !shortPress ) {
            if ( counter >= SHORT_PRESS_DURATION ) {
                shortPress = 1;
            }
        } else {
            if ( counter >= LONG_PRESS_DURATION ) {
                longPress = 1;
                break;
            }
        }
        counter++;
        _delay_ms(50);
    }

    if ( !shortPress ) {
        return;
    }

    switch(state) {
        case NO_EDIT:       state = ENABLED_EDIT;      break;
        case ENABLED_EDIT:  state = FREQ_EDIT;         break;
        case FREQ_EDIT:     if (longPress) {
                                frequencyEditChar = FREQ_EDITOR_WIDTH;
                                state = FREQ_SYM_EDIT;
                            } else {
                                state = WAVEFORM_EDIT;
                            };
                                                       break;
        case FREQ_SYM_EDIT: if (longPress) {
                                state = FREQ_EDIT;
                            } else {
                                if ( frequencyEditChar == FREQ_EDITOR_WIDTH - FREQ_EDITOR_FRACT + 1 ) { // check the dot
                                   frequencyEditChar = frequencyEditChar - 2;
                                } else {
                                    if ( frequencyEditChar > 1 ) {
                                       frequencyEditChar--;   
                                    } else {
                                       frequencyEditChar = FREQ_EDITOR_WIDTH;
                                    }
                                }
                                state = FREQ_SYM_EDIT;
                            };                         break;
        case WAVEFORM_EDIT: state = REMOVEDC_EDIT;     break;
        case REMOVEDC_EDIT: state = AMP_EDIT;          break;
        case AMP_EDIT:      if (longPress) {
                                ampEditChar = AMP_EDITOR_WIDTH;
                                state = AMP_SYM_EDIT;
                            } else {
                                state = SAVE_EDIT;
                            };                         break;
        case AMP_SYM_EDIT:  if (longPress) {
                                state = AMP_EDIT;
                            } else {
                                if ( ampEditChar == AMP_EDITOR_WIDTH - AMP_EDITOR_FRACT + 1 ) { // check the dot
                                   ampEditChar = ampEditChar - 2;
                                } else {
                                    if ( ampEditChar > 1 ) {
                                       ampEditChar--;   
                                    } else {
                                       ampEditChar = AMP_EDITOR_WIDTH;
                                    }
                                }
                                state = AMP_SYM_EDIT;
                            };                         break;
       case SAVE_EDIT:      if (longPress) {
                                saveSettings();
                                state = NO_EDIT;
                            } else {
                                state = NO_EDIT;
                            };                         break;
    }

    scheduleScreenUpdate = 1;
}

void redrawScreen() {
    char str_buff[20], data_buff[12];

    u8g2.firstPage();
    do {
        u8g2.setFont(u8g2_font_micro_mr);
        // output enable status
        sprintf( str_buff, "Output enabled: %s", outputEnabled ? "YES" : "NO" );
        u8g2.setDrawColor( state == ENABLED_EDIT ? 0 : 1 );
        u8g2.drawStr( 0, 5, str_buff );

        // actual frequency
        float actualFrequency = gen.GetActualProgrammedFrequency(REG0);
        dtostrf( actualFrequency, FREQ_EDITOR_WIDTH, FREQ_EDITOR_FRACT, data_buff );
        sprintf( str_buff, "Freq.: %s Hz", data_buff );
        u8g2.setDrawColor( state == FREQ_EDIT ? 0 : 1 );
        u8g2.drawStr( 0, 13, str_buff );
        if ( state == FREQ_SYM_EDIT ) {
            u8g2.drawBox( 23 + 4 * frequencyEditChar, 14, 5, 1 );  
        }

        // waveform
        switch(waveform) {
            case SINE_WAVE:        strncpy( data_buff, "SINE",     10 ); break;
            case TRIANGLE_WAVE:    strncpy( data_buff, "TRIANGLE", 10 ); break;
            case SQUARE_WAVE:      strncpy( data_buff, "SQUARE",   10 ); break;
            case HALF_SQUARE_WAVE: strncpy( data_buff, "SQUARE/2", 10 ); break;
        }
        sprintf( str_buff, "Waveform: %s", data_buff );
        u8g2.setDrawColor( state == WAVEFORM_EDIT ? 0 : 1 );
        u8g2.drawStr( 0, 21, str_buff );

        // dc offset removal status
        sprintf( str_buff, "Remove DC offset: %s", removeDcOffset ? "YES" : "NO" );
        u8g2.setDrawColor( state == REMOVEDC_EDIT ? 0 : 1 );
        u8g2.drawStr( 0, 29, str_buff );

        // amplification
        dtostrf( amplification, AMP_EDITOR_WIDTH, AMP_EDITOR_FRACT, data_buff );
        sprintf( str_buff, "Amplifier: %s %%", data_buff );
        u8g2.setDrawColor( state == AMP_EDIT ? 0 : 1 );
        u8g2.drawStr( 0, 37, str_buff );
        if ( state == AMP_SYM_EDIT ) {
            u8g2.drawBox( 39 + 4 * ampEditChar, 38, 5, 1 );  
        }

        // save settings
        u8g2.setDrawColor( state == SAVE_EDIT ? 0 : 1 );
        u8g2.drawStr( 0, 45, "Save settings" );
    } while ( u8g2.nextPage() );
}

void updateDcOffsetRemoval() {
    digitalWrite( DC_RELAY_PIN, !removeDcOffset );
    scheduleScreenUpdate = 1;
}

void updateWaveform() {
    switch(waveform) {
        case SINE_WAVE:        break;
        case TRIANGLE_WAVE:    break;
        case SQUARE_WAVE:      break;
        case HALF_SQUARE_WAVE: break;
        default: waveform = SINE_WAVE;
    }
    gen.SetWaveform( ACTIVE_REGISTER, waveform );
    scheduleScreenUpdate = 1;
}

void updateFrequency() {
    if ( frequency > FREQ_EDITOR_MAX) {
        frequency = FREQ_EDITOR_MAX;
    } else if ( frequency < FREQ_EDITOR_MIN) {
        frequency = FREQ_EDITOR_MIN;
    }
    gen.SetFrequency( ACTIVE_REGISTER, frequency );
    scheduleScreenUpdate = 1;
}

void updateOutput() {
    gen.EnableOutput(outputEnabled);
    scheduleScreenUpdate = 1;
}

void updateAmplification() {
    if ( amplification > AMP_EDITOR_MAX) {
        amplification = AMP_EDITOR_MAX;
    } else if (amplification < AMP_EDITOR_MIN) {
        amplification = AMP_EDITOR_MIN;
    }
    SPI.beginTransaction( SPISettings( 4000000, MSBFIRST, SPI_MODE0 ) );
    digitalWrite( MCP_CS_PIN, LOW );
    SPI.transfer( B00010001 );
    SPI.transfer( (uint8_t)( 2.55 * amplification ) ); 
    digitalWrite( MCP_CS_PIN, HIGH );
    SPI.endTransaction();
    scheduleScreenUpdate = 1;
}

void saveSettings() {
    int eeAddress = 0;

    EEPROM.put( eeAddress, frequency );
    eeAddress += sizeof frequency;

    EEPROM.put( eeAddress, waveform );
    eeAddress += sizeof waveform;

    EEPROM.put( eeAddress, removeDcOffset );
    eeAddress += sizeof removeDcOffset;

    EEPROM.put( eeAddress, outputEnabled );
    eeAddress += sizeof outputEnabled;

    EEPROM.put( eeAddress, amplification );
}

void restoreSettings() {
    int eeAddress = 0;
    EEPROM.get( eeAddress, frequency );
    updateFrequency();

    eeAddress += sizeof frequency;
    EEPROM.get( eeAddress, waveform );
    updateWaveform();

    eeAddress += sizeof waveform;
    EEPROM.get( eeAddress, removeDcOffset );
    updateDcOffsetRemoval();

    eeAddress += sizeof removeDcOffset;
    EEPROM.get( eeAddress, outputEnabled );
    updateOutput();

    eeAddress += sizeof outputEnabled;
    EEPROM.get( eeAddress, amplification );
    updateAmplification();
}

