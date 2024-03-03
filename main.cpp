#include <cstdint>
#include <stdint.h>

#include "third_party/nugget/common/syscalls/syscalls.h"
#include "third_party/nugget/psyqo/alloc.h"
#include "third_party/nugget/psyqo/application.hh"
#include "third_party/nugget/psyqo/font.hh"
#include "third_party/nugget/psyqo/gpu.hh"
#include "third_party/nugget/psyqo/simplepad.hh"
#include "third_party/nugget/psyqo/scene.hh"
#include "third_party/nugget/psyqo/xprintf.h"

#include "third_party/nugget/common/hardware/dma.h"
#include "third_party/nugget/common/hardware/spu.h"

#include "sound.h"

namespace {

//------------------------------------------------------------------------------------------------------------
// A PSYQo software needs to declare one \`Application\` object.
// This is the one we're going to do for our hello world.
class Hello final : public psyqo::Application
{

    void prepare() override;
    void createScene() override;

  public:
    psyqo::Font<> m_font;
    psyqo::SimplePad m_input;
};

//------------------------------------------------------------------------------------------------------------
// And we need at least one scene to be created.
// This is the one we're going to do for our hello world.
class HelloScene final : public psyqo::Scene
{
    void frame() override;

    // We'll have some simple animation going on, so we
    // need to keep track of our state here.
    uint8_t m_anim = 0;
    bool m_direction = true;
};

//------------------------------------------------------------------------------------------------------------
// We're instantiating the two objects above right now.
Hello hello;
HelloScene helloScene;

}  // namespace

//------------------------------------------------------------------------------------------------------------
void Hello::createScene()
{
    m_font.uploadSystemFont(gpu());
    m_input.initialize();
    pushScene(&helloScene);
}

//------------------------------------------------------------------------------------------------------------
// ADSR Parameters
//------------------------------------------------------------------------------------------------------------
enum class EParameters : uint8_t
{
    AttackMode,
    AttackShift,
    AttackStep,
    DecayShift,
    SustainLevel,
    SustainMode,
    SustainDir,
    SustainShift,
    SustainStep,
    ReleaseMode,
    ReleaseShift,
    Waveform,
    NUM
};

//------------------------------------------------------------------------------------------------------------
struct FParameterRange
{
    uint8_t m_Min = 0x00;
    uint8_t m_Max = 0x0F;
} g_ParameterRanges[ (uint8_t)EParameters::NUM ] = {
    { 0x00, 0x01 }, // AttackMode
    { 0x00, 0x1F }, // AttackShift
    { 0x00, 0x03 }, // AttackStep
    { 0x00, 0x0F }, // DecayShift
    { 0x00, 0x0F }, // SustainLevel
    { 0x00, 0x01 }, // SustainMode
    { 0x00, 0x01 }, // SustainDir
    { 0x00, 0x1F }, // SustainShift
    { 0x00, 0x03 }, // SustainStep
    { 0x00, 0x01 }, // ReleaseMode
    { 0x00, 0x1F }, // ReleaseShift
    { 0x00, 0x01 }, // Waveform
};

//------------------------------------------------------------------------------------------------------------
uint8_t g_CurrentParameterValues[ (uint8_t)EParameters::NUM ] = {
    0x00, // AttackShift
    0x00, // AttackShift
    0x00, // AttackStep
    0x0F, // DecayShift
    0x0F, // SustainLevel
    0x00, // SustainMode
    0x00, // SustainDir
    0x00, // SustainShift
    0x00, // SustainStep
    0x00, // ReleaseMode
    0x00, // ReleaseShift
    0x00, // Waveform
};

//------------------------------------------------------------------------------------------------------------
const char* ParameterNames[(uint8_t)EParameters::NUM] = {
    "Attack Mode",
    "Attack Shift",
    "Attack Step",
    "Decay Shift",
    "Sustain Level",
    "Sustain Mode",
    "Sustain Dir",
    "Sustain Shift",
    "Sustain Step",
    "Release Mode",
    "Release Shift",
    "Waveform",
};

EParameters g_CurrentSelection = EParameters::AttackMode;

//------------------------------------------------------------------------------------------------------------
// SPU Helpers
//------------------------------------------------------------------------------------------------------------
struct SpuInstrumentData
{
    uint16_t baseAddress;
    uint8_t finetune;
    uint8_t volume;
};
static struct SpuInstrumentData s_spuInstrumentData[31];

void SPUInit()
{
    DPCR |= 0x000b0000;
    SPU_VOL_MAIN_LEFT = 0x3800;
    SPU_VOL_MAIN_RIGHT = 0x3800;
    SPU_CTRL = 0;
    SPU_KEY_ON_LOW = 0;
    SPU_KEY_ON_HIGH = 0;
    SPU_KEY_OFF_LOW = 0xffff;
    SPU_KEY_OFF_HIGH = 0xffff;
    SPU_RAM_DTC = 4;
    SPU_VOL_CD_LEFT = 0;
    SPU_VOL_CD_RIGHT = 0;
    SPU_PITCH_MOD_LOW = 0;
    SPU_PITCH_MOD_HIGH = 0;
    SPU_NOISE_EN_LOW = 0;
    SPU_NOISE_EN_HIGH = 0;
    SPU_REVERB_EN_LOW = 0;
    SPU_REVERB_EN_HIGH = 0;
    SPU_VOL_EXT_LEFT = 0;
    SPU_VOL_EXT_RIGHT = 0;
    SPU_CTRL = 0x8000;
}

static void SPUResetVoice(int voiceID)
{
    SPU_VOICES[voiceID].volumeLeft = 0;
    SPU_VOICES[voiceID].volumeRight = 0;
    SPU_VOICES[voiceID].sampleRate = 0;
    // SPU_VOICES[voiceID].sampleStartAddr = Instrument::SINE_SIZE + 1000;
    SPU_VOICES[voiceID].sampleStartAddr = 0;
    SPU_VOICES[voiceID].ad = 0x000f;
    SPU_VOICES[voiceID].currentVolume = 0;
    // SPU_VOICES[voiceID].sampleRepeatAddr = Instrument::SINE_SIZE + 1000;
    SPU_VOICES[voiceID].sampleRepeatAddr = 0;
    SPU_VOICES[voiceID].sr = 0x0000;
}

void SPUUploadInstruments(uint32_t SpuAddr, const uint8_t* data, uint32_t size)
{
    uint32_t bcr = size >> 6;
    if (size & 0x3f) bcr++;
    bcr <<= 16;
    bcr |= 0x10;

    SPU_RAM_DTA = SpuAddr >> 3;
    SPU_CTRL = (SPU_CTRL & ~0x0030) | 0x0020;
    while ((SPU_CTRL & 0x0030) != 0x0020)
        ;
    // original code erroneously was doing SBUS_DEV4_CTRL = SBUS_DEV4_CTRL;
    SBUS_DEV4_CTRL &= ~0x0f000000;
    DMA_CTRL[DMA_SPU].MADR = (uint32_t)data;
    DMA_CTRL[DMA_SPU].BCR = bcr;
    DMA_CTRL[DMA_SPU].CHCR = 0x01000201;

    while ((DMA_CTRL[DMA_SPU].CHCR & 0x01000000) != 0)
        ;
}

void SPUUnMute() { SPU_CTRL = 0xc000; }

static uint32_t s_masterVolume = 16384;

static void SPUSetVoiceVolume(int voiceID, uint32_t left, uint32_t right)
{
    SPU_VOICES[voiceID].volumeLeft = (left * s_masterVolume) >> 16;
    SPU_VOICES[voiceID].volumeRight = (right * s_masterVolume) >> 16;
}

static void SPUSetStartAddress(int voiceID, uint32_t spuAddr) { SPU_VOICES[voiceID].sampleStartAddr = spuAddr >> 3; }

void SPUWaitIdle()
{
    do {
        for (unsigned c = 0; c < 2045; c++) __asm__ volatile("");
    } while ((SPU_STATUS & 0x07ff) != 0);
}

//------------------------------------------------------------------------------------------------------------
// All this shit
//------------------------------------------------------------------------------------------------------------
/*
    ____lower 16bit (at 1F801C08h+N*10h)___________________________________
    15    Attack Mode       (0=Linear, 1=Exponential)
    -     Attack Direction  (Fixed, always Increase) (until Level 7FFFh)
    14-10 Attack Shift      (0..1Fh = Fast..Slow)
    9-8   Attack Step       (0..3 = "+7,+6,+5,+4")
    -     Decay Mode        (Fixed, always Exponential)
    -     Decay Direction   (Fixed, always Decrease) (until Sustain Level)
    7-4   Decay Shift       (0..0Fh = Fast..Slow)
    -     Decay Step        (Fixed, always "-8")
    3-0   Sustain Level     (0..0Fh)  ;Level=(N+1)*800h
    ____upper 16bit (at 1F801C0Ah+N*10h)___________________________________
    31    Sustain Mode      (0=Linear, 1=Exponential)
    30    Sustain Direction (0=Increase, 1=Decrease) (until Key OFF flag)
    29    Not used?         (should be zero)
    28-24 Sustain Shift     (0..1Fh = Fast..Slow)
    23-22 Sustain Step      (0..3 = "+7,+6,+5,+4" or "-8,-7,-6,-5") (inc/dec)
    21    Release Mode      (0=Linear, 1=Exponential)
    -     Release Direction (Fixed, always Decrease) (until Level 0000h)
    20-16 Release Shift     (0..1Fh = Fast..Slow)
    -     Release Step      (Fixed, always "-8")
*/

#define ATTACK_MODE   15
#define ATTACK_SHIFT  10
#define ATTACK_STEP    8
#define DECAY_SHIFT    4
#define SUSTAIN_LEVEL  0

#define SUSTAIN_MODE  31
#define SUSTAIN_DIR   30
#define UNUSED        29
#define SUSTAIN_SHIFT 24
#define SUSTAIN_STEP  22
#define RELEASE_MODE  21
#define RELEASE_SHIFT 16

//------------------------------------------------------------------------------------------------------------
void UpdateADSR()
{
    uint8_t voiceID = 0;

    uint32_t ADSR = 0;
    ADSR |= g_CurrentParameterValues[ (uint8_t)EParameters::AttackMode   ] << ATTACK_MODE;
    ADSR |= g_CurrentParameterValues[ (uint8_t)EParameters::AttackShift  ] << ATTACK_SHIFT;
    ADSR |= g_CurrentParameterValues[ (uint8_t)EParameters::AttackStep   ] << ATTACK_STEP;
    ADSR |= g_CurrentParameterValues[ (uint8_t)EParameters::DecayShift   ] << DECAY_SHIFT;
    ADSR |= g_CurrentParameterValues[ (uint8_t)EParameters::SustainLevel ] << SUSTAIN_LEVEL;
    ADSR |= g_CurrentParameterValues[ (uint8_t)EParameters::SustainMode  ] << SUSTAIN_MODE;
    ADSR |= g_CurrentParameterValues[ (uint8_t)EParameters::SustainDir   ] << SUSTAIN_DIR;
    ADSR |= g_CurrentParameterValues[ (uint8_t)EParameters::SustainShift ] << SUSTAIN_SHIFT;
    ADSR |= g_CurrentParameterValues[ (uint8_t)EParameters::SustainStep  ] << SUSTAIN_STEP;
    ADSR |= g_CurrentParameterValues[ (uint8_t)EParameters::ReleaseMode  ] << RELEASE_MODE;
    ADSR |= g_CurrentParameterValues[ (uint8_t)EParameters::ReleaseShift ] << RELEASE_SHIFT;

    SPU_VOICES[voiceID].ad = ADSR & 0xFFFF;
    SPU_VOICES[voiceID].sr = ADSR >> 16;
}

//------------------------------------------------------------------------------------------------------------
static void OnButtonPress( psyqo::SimplePad::Event in_Event )
{
    using Button = psyqo::SimplePad::Button;
    using Event = psyqo::SimplePad::Event;
    if( in_Event.type == Event::ButtonPressed )
    {
        switch( in_Event.button )
        {
        case Button::Up:
            g_CurrentSelection = g_CurrentSelection == (EParameters)0 ? 
                (EParameters)( (uint8_t)EParameters::NUM - 1 ) : 
                (EParameters)( ( (uint8_t)g_CurrentSelection - 1) % (uint8_t)EParameters::NUM );
            break;
        case Button::Down:
            g_CurrentSelection = (EParameters)( ((uint8_t)g_CurrentSelection + 1) % (uint8_t)EParameters::NUM);
            break;
        case Button::Left: {
                uint8_t& Value = g_CurrentParameterValues[ (uint8_t)g_CurrentSelection ];
                FParameterRange& Range = g_ParameterRanges[ (uint8_t)g_CurrentSelection ];
                if( Value != Range.m_Min)
                {
                    Value--; 
                    UpdateADSR();
                }
            } break;
        case Button::Right: {
                uint8_t& Value = g_CurrentParameterValues[ (uint8_t)g_CurrentSelection ];
                FParameterRange& Range = g_ParameterRanges[ (uint8_t)g_CurrentSelection ];
                if( Value != Range.m_Max)
                {
                    Value++; 
                    UpdateADSR();
                }
            } break;
        case Button::Cross: {
                SPUWaitIdle();
                uint8_t voiceID = 0;
                SPU_KEY_ON_LOW = 1 << voiceID;
            } break;
        case Button::Circle: {
                SPUWaitIdle();
                uint8_t voiceID = 0;
                SPU_KEY_OFF_LOW = 1 << voiceID;
            } break;
        default:
            break;
        }
    }
}

//------------------------------------------------------------------------------------------------------------
void Hello::prepare()
{
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W320)
        .set(psyqo::GPU::VideoMode::AUTO)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);

    m_input.setOnEvent( &OnButtonPress );

    uint8_t voiceID = 0;
    uint8_t instrumentID = 0;

    SPUInit();
    // TODO(jperos): Build a waveform bank for multiple waveforms
    // uint32_t WaveformBankSize = Instrument::GUITAR_SIZE;
    // uint8_t* WaveformBank = (uint8_t*)psyqo_malloc( WaveformBankSize );
    // __builtin_memcpy( WaveformBank, Instrument::GUITAR, Instrument::GUITAR_SIZE );
    SPUUploadInstruments(0x1010, Instrument::SINE, Instrument::SINE_SIZE );
    SPUUnMute();

    SPUResetVoice( voiceID );

    uint8_t volume = 63;
    SPUSetVoiceVolume(0, volume << 8, volume << 8);
    // SPUSetStartAddress( 0, 0x1010 );

    UpdateADSR();

    // SPUResetVoice
    // SPU_VOICES[voiceID].sampleRate = 0xb9e; // guitar sound is at 32.022kHz
    SPU_VOICES[voiceID].sampleRate = 0x800; // sine wave is at 22.050kHz
    SPU_VOICES[voiceID].currentVolume = 0;
    SPU_VOICES[voiceID].sampleRepeatAddr = 0x1010 + 5000; // Just trying some shit
}

//------------------------------------------------------------------------------------------------------------
uint32_t strlen( const char* in_String )
{
    uint32_t i = 0;
    while( in_String[i] != '\0' )
    {
        i++;
    }
    return i;
}

//------------------------------------------------------------------------------------------------------------
// Formatting constants
constexpr uint32_t LINE_SPACING = 16;
constexpr uint32_t INDENTATION = 16;

//------------------------------------------------------------------------------------------------------------
void HelloScene::frame()
{
    // Background animation
    if (m_anim == 0)
    {
        m_direction = true;
    }
    else if (m_anim == 255)
    {
        m_direction = false;
    }
    psyqo::Color bg{{.r = 0, .g = 64, .b = 91}};
    bg.r = m_anim;
    hello.gpu().clear(bg);
    if (m_direction)
    {
        m_anim++;
    }
    else
    {
        m_anim--;
    }

    // Print all the shit on screen
    psyqo::Color c = {{.r = 255, .g = 255, .b = uint8_t(255 - m_anim)}};
    for( uint8_t i = 0; i < (uint8_t)EParameters::NUM; i++ )
    {
        const char*& ParameterName = ParameterNames[i];
        const uint8_t ParameterValue = g_CurrentParameterValues[i];
        const char* Prefix = i == (uint8_t)g_CurrentSelection ? ">" : " ";
        psyqo::Vertex Position;
        Position.x = INDENTATION;
        Position.y = LINE_SPACING * ( i + 1 );
        hello.m_font.printf(hello.gpu(), Position, c, "%s %-25s 0x%02X", Prefix, ParameterName, ParameterValue );
    }
}

//------------------------------------------------------------------------------------------------------------
int main() {
    return hello.run();
}
