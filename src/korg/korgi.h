#pragma once

/*   
* Need Korg nanoKontrol 2 MIDI unit connected to the machine at applciation launch time.
*
* Define globals in your code like so:
*		KorgKnob g_exampleKnob("Example", CHANNEL_ID, &gKnobValue, 0.0f, 1.0f); Current Value, Min Value, Max Value
*		KorgButton g_exampleButton("Example", CHANNEL_ID, KorgButtonMode::Toggle, &gToggleValue);
* 
* CHANNEL_ID can be replaced with whatever mappign you desire.
* Multiple parameters can be hooked to the same controlChannel, and the controller will update all simultaneously.
*/

struct IKorgI
{
	virtual bool init() = 0;
	virtual void shutdown() = 0;
    virtual void update() = 0;
	virtual void addHook(unsigned char controlChannel, struct KorgButton* pParam) = 0;
	virtual void addHook(unsigned char controlChannel, struct KorgKnob* pParam) = 0;
};

extern IKorgI* getKorgI();

enum class KorgButtonMode
{
    Momentary, // Use KorgButton::wasMomentarilyPressed() to get a single 'true' for each press.
    Toggle
};

struct KorgButton
{
	KorgButton(const char* name, int page, unsigned char controlChannel, KorgButtonMode mode, bool* pValue = nullptr)
        : mName(name)
        , mMode(mode)
        , mpValue(pValue ? pValue : &mLocalValue)
        , mPreviousValue(false)
        , mLocalValue(false)
        , mLedStatus(*mpValue)
        , mPage(page)
	{
		mDefaultValue = *mpValue;
		getKorgI()->addHook(controlChannel, this);
	}

    // Returns true only once as the button is pressed
    // (will not continue to return true as the button is held)
    bool wasMomentarilyPressed()
    {
        bool retVal = false;
        if(*mpValue && !mPreviousValue)
        {
            retVal = true;
        }
        // Clear the previous value, so this function only returns true once.
        mPreviousValue = *mpValue;
        return retVal;
    }

    bool* getValuePtr() const {
        return mpValue;
    }

	bool getDefaultValue() const {
		return mDefaultValue;
	}

    const std::string& getName() const {
        return mName;
    }

    KorgButtonMode getMode() const {
        return mMode;
    }

private:
    friend struct KorgI;

    bool getValue() const { return *mpValue; }
    void setValue(bool value) { *mpValue = value; }
    bool getLedStatus() const { return mLedStatus;  }
    void setLedStatus(bool status) { mLedStatus = status; }
    int getPage() const { return mPage; }

	std::string mName;
    KorgButtonMode mMode;
    bool* mpValue;
	bool mDefaultValue;
    bool mPreviousValue;
    bool mLocalValue; // If pValue initialised as nullptr
    bool mLedStatus;
    int mPage;
};

struct KorgKnob
{
	KorgKnob(const char* name, int page, unsigned char controlChannel, float* pValue, float mi = 0.0f, float ma = 1.0f)
        : mName(name), mpValue(pValue), mDefaultValue(*pValue), min_value(mi), max_value(ma), mPage(page)
	{
		getKorgI()->addHook(controlChannel, this);
	}

    float* getValuePtr() const {
        return mpValue;
    }

	float getDefaultValue() const {
		return mDefaultValue;
	}

    const std::string& getName() const {
        return mName;
    }

private:
    friend struct KorgI;

	void setValue(const float newRawValue)
	{
        *mpValue = min_value * (1.f - newRawValue) + max_value * newRawValue;
	}
    int getPage() const { return mPage; }

	std::string mName;
    float* mpValue;
	float mDefaultValue;
	float min_value;
	float max_value;
    int mPage;
};
