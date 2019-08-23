#include "pch.h"

#include <stdio.h>
#include <thread>
#include <WinSock2.h>
#include <mmsystem.h>
#include <WS2tcpip.h>

#include <unordered_map>
#include <algorithm>
#include <signal.h>

#include "korgi.h"

#ifdef _WIN32
#pragma comment(lib, "winmm")
#pragma comment(lib, "ws2_32")
#endif

using namespace std;

bool sPageBit0 = false;
bool sPageBit1 = false;
static KorgButton sPageButton0("Page0", -1, 44, KorgButtonMode::Toggle, &sPageBit0);
static KorgButton sPageButton1("Page1", -1, 43, KorgButtonMode::Toggle, &sPageBit1);

struct KorgI : public IKorgI
{
	void addHook(unsigned char controlChannel, KorgKnob* pParam) override
	{
		knobs[controlChannel].push_back(pParam);
	}

	void addHook(unsigned char controlChannel, KorgButton* pParam) override
	{
		buttons[controlChannel].push_back(pParam);
        setLedStatus(controlChannel, pParam);
    }

	bool init() override
	{
		if (!OpenMidiDevice())
			return false;

		return true;
	}

	void shutdown() override
	{
		CloseMidiDevice();
	}

    virtual void update() override
    {
        int currentPage = (sPageBit0 ? 1 : 0) | (sPageBit1 ? 2 : 0);
        if(currentPage != m_currentPage)
        {
            m_currentPage = currentPage;
            setAllLeds();
        }
        else
        {
            // Update the status of LEDs if the code has changed any of the button values
            for(const auto& it0 : buttons)
            {
                int cc = it0.first;
                for(KorgButton* pButton : it0.second)
                {
                    if(((pButton->getPage() == -1) || (pButton->getPage() == m_currentPage))
                        && (pButton->getLedStatus() != pButton->getValue()))
                    {
                        setLedStatus(cc, pButton);
                    }
                }
            }
        }
    }

	virtual ~KorgI() {}

private:
	//static const string device_name = "nanoKONTROL2";

	HMIDIIN  m_midiInHandle = {};
    HMIDIOUT m_midiOutHandle = {};
    int m_deviceIdx = 0;
    int m_currentPage = 0;

	unordered_map<int, std::vector<KorgKnob*>>   knobs;
	unordered_map<int, std::vector<KorgButton*>> buttons;

	static void CALLBACK MidiInCallback(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2);

	void HandleMidiInput(unsigned char controlChannel, unsigned char midiValue)
	{
		auto button = buttons.find(controlChannel);
		auto knob = knobs.find(controlChannel);

		if (button != buttons.end())
		{
			for (auto b : button->second)
			{
                if((b->getPage() == -1) || (b->getPage() == m_currentPage))
                {
                    bool isPressed = (midiValue > 0);
                    switch(b->getMode())
                    {
                    case KorgButtonMode::Momentary:
                        // Set the value to the current state of the button
                        b->setValue(isPressed);
                        setLedStatus(controlChannel, b);
                        break;
                    case KorgButtonMode::Toggle:
                        if(isPressed)
                        {
                            // Toggle the button
                            if(b->getValue())
                            {
                                // Turn off
                                b->setValue(false);
                                setLedStatus(controlChannel, b);
                            }
                            else
                            {
                                // Turn on
                                b->setValue(true);
                                setLedStatus(controlChannel, b);
                            }
                        }
                        break;
                    }
                }
			}
		}
		else if (knob != knobs.end())
		{
			float fvalue = (float)midiValue / 127.f;
			fvalue = max(0.f, min(1.f, fvalue));
			
			for (auto k : knob->second)
			{
                if((k->getPage() == -1) || (k->getPage() == m_currentPage))
                {
                    k->setValue(fvalue);
                }
			}
		}
	}

	bool OpenMidiDevice()
	{
		if (midiInOpen(&m_midiInHandle, m_deviceIdx, (DWORD_PTR)MidiInCallback, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
		{
			return false;
		}

		midiInStart(m_midiInHandle);

		// Try to open the nanoKONTROL2 as an output device
        uint32_t numOutputDevices = midiOutGetNumDevs();
        for(uint32_t i = 0; i < numOutputDevices; ++i)
        {
            MIDIOUTCAPS caps;
            midiOutGetDevCaps(i, &caps, sizeof(MIDIOUTCAPS));
            printf(caps.szPname);
            if(strncmp(caps.szPname, "nanoKONTROL2", strlen("nanoKONTROL2")) == 0)
            {
                if(midiOutOpen(&m_midiOutHandle, i, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR)
                {
                    // Set the initial status of the LEDs
                    setAllLeds();
                }
                break;
            }
        }

		MIDIINCAPS inCaps = {};
		MMRESULT res = midiInGetDevCaps((UINT_PTR)&m_deviceIdx, &inCaps, sizeof(MIDIINCAPS));

        // OWRIGHT : We get MMSYSERR_BADDEVICEID, but it still works.
        return (res == MMSYSERR_NOERROR) || (res == MMSYSERR_BADDEVICEID);
	}

	void CloseMidiDevice()
	{
		midiInClose(m_midiInHandle);
		m_midiInHandle = 0;
	}

    void setAllLeds()
    {
        for(const auto& it0 : buttons)
        {
            int cc = it0.first;
            for(KorgButton* pButton : it0.second)
            {
                if((pButton->getPage() == -1) || (pButton->getPage() == m_currentPage))
                {
                    setLedStatus(cc, pButton);
                }
            }
        }
    }

    void setLedStatus(unsigned char controlChannel, KorgButton* pButton)
    {
        if(m_midiOutHandle)
        {
            union {
                DWORD dwData;
                BYTE bData[4];
            } u;
            const uint8_t kMidiChannel = 0;
            u.bData[0] = 0xb0/*control change*/ | kMidiChannel;  // MIDI status byte 
            u.bData[1] = controlChannel;  // first MIDI data byte  : CC number
            u.bData[2] = pButton->getValue() ? 127 : 0; // second MIDI data byte : Value
            u.bData[3] = 0;
            midiOutShortMsg(m_midiOutHandle, u.dwData);
            pButton->setLedStatus(pButton->getValue());
        }
    }
};

static KorgI* s_pKorg = nullptr;
IKorgI* getKorgI()
{
    if(!s_pKorg)
    {
        s_pKorg = new KorgI();
    }
    return s_pKorg;
}

void CALLBACK KorgI::MidiInCallback(HMIDIIN  hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
	if (wMsg != MIM_DATA)
		return;

	char controlChannel = (dwParam1 >> 8) & 0xff;
	char midiValue = (dwParam1 >> 16) & 0xff;

    s_pKorg->HandleMidiInput(controlChannel, midiValue);
}
