#include "gain_control.h"

void initAGC(AGC* agc, uint32_t sampleRate, float targetLevel, float minGain, float maxGain, float attackTime, float releaseTime) {
    agc->targetLevel = targetLevel;
    agc->minGain = minGain;
    agc->maxGain = maxGain;

    agc->attackCoef = expf(-1.0f / (sampleRate * attackTime));
    agc->releaseCoef = expf(-1.0f / (sampleRate * releaseTime));
    agc->rmsAlpha = expf(-1.0f / (sampleRate * 0.025f));
    agc->rmsBeta = 1.0f - agc->rmsAlpha;
    agc->sampleRate = sampleRate;

    agc->rmsBuffer = 0.0f;
    agc->currentGain = 1.0f;
    agc->currentLevel = 0.0f;
}

float process_agc(AGC* agc, float sidechain) {
    const float signalPower = sidechain * sidechain;

    agc->rmsBuffer = agc->rmsAlpha * agc->rmsBuffer + agc->rmsBeta * signalPower;
    
    const float rmsLevel = sqrtf(agc->rmsBuffer);
    
    const float levelAlpha = (rmsLevel > agc->currentLevel) ? agc->attackCoef : agc->releaseCoef;
    agc->currentLevel = levelAlpha * agc->currentLevel + (1.0f - levelAlpha) * rmsLevel;

    float desiredGain = agc->targetLevel / (agc->currentLevel + 1e-9f);

    desiredGain = fminf(fmaxf(desiredGain, agc->minGain), agc->maxGain);

    const float gainAlpha = (desiredGain < agc->currentGain) ? agc->attackCoef : agc->releaseCoef;
    agc->currentGain = gainAlpha * agc->currentGain + (1.0f - gainAlpha) * desiredGain;

    return agc->currentGain;
}