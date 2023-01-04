/*
#-------------------------------------------------------------------------------
# Copyright (c) 2023 Alexander Couzens
# All rights reserved. This program and the accompanying materials
# are made available under the terms of the GNU Public License v3.0
# which accompanies this distribution, and is available at
# http://www.gnu.org/licenses/gpl.html
# 
# Contributors:
#     Alexander Couzens <lynxis@fe80.eu>
#
# based on UHD driver
#-------------------------------------------------------------------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>


#include <iostream>
#include <complex>

#include "TSDRPlugin.h"

#include "TSDRCodes.h"

#include <stdint.h>

#include "errors.hpp"

#define HOW_OFTEN_TO_CALL_CALLBACK_SEC (0.06)
#define FRACT_DROPPED_TO_TOLERATE (0)

/* struct based on GPLv2 RTLSDR-Airband */
typedef struct {
	SoapySDR::Device *dev;	// pointer to device struct
	char const *device_string;	// SoapySDR device arg string
	char const *sample_format;	// sample format
	char const *antenna;		// antenna name
	SoapySDRKwargs gains;		// gain elements and their values
	double correction;	// PPM correction
	double gain;		// gain in dB
	size_t channel;		// HW channel number
	std::vector<size_t> channels;
	bool agc;		// enable AGC
} soapysdr_dev_data_t;

soapysdr_dev_data_t dev_data = { .sample_format = "CF32", .channel = 0 };

uint32_t req_freq = 105e6;
double req_rate = 25e6;
volatile int is_running = 0;

EXTERNC TSDRPLUGIN_API void __stdcall tsdrplugin_getName(char * name) {
	strcpy(name, "TSDR soapy Compatible Plugin");
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_init(const char * params) {
	// simulate argv and argc
	std::string sparams(params);

	typedef std::vector< std::string > split_vector_type;
	try {
		//create a usrp device
		dev_data.device_string = params;
		dev_data.dev = SoapySDR::Device::make(dev_data.device_string);
		if (dev_data.dev == NULL) {
                std::cout << "Couldn't create soapy device out of the device string" << params << std::endl;
			return TSDR_CANNOT_OPEN_DEVICE;
		}

		dev_data.channels.push_back(0);
	} catch (std::exception const&  ex)
	{
		RETURN_EXCEPTION(ex.what(), TSDR_CANNOT_OPEN_DEVICE);
	}

	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API uint32_t __stdcall tsdrplugin_setsamplerate(uint32_t rate) {
	uint32_t sample_rate = 0;
	if (is_running)
		return tsdrplugin_getsamplerate();

	try {
		dev_data.dev->setSampleRate(SOAPY_SDR_RX, dev_data.channel, (double) rate);
		double real_rate = dev_data.dev->getSampleRate(SOAPY_SDR_RX, dev_data.channel);
		sample_rate = (uint32_t) real_rate;
	}
	catch (std::exception const&  ex)
	{
	}

	return sample_rate;
}

EXTERNC TSDRPLUGIN_API uint32_t __stdcall tsdrplugin_getsamplerate() {
	uint32_t sample_rate = 0;
	try {
		sample_rate = (uint32_t) dev_data.dev->getSampleRate(SOAPY_SDR_RX, dev_data.channel);
	}
	catch (std::exception const&  ex)
	{
	}

	return sample_rate;
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_setbasefreq(uint32_t freq) {
	try {
		dev_data.dev->setFrequency(SOAPY_SDR_RX, dev_data.channel, (double) freq);
	}
	catch (std::exception const&  ex)
	{
	}
	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_stop(void) {
	is_running = 0;
	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_setgain(float gain) {
	dev_data.gain = gain;
	try {
		dev_data.dev->setGain(SOAPY_SDR_RX, dev_data.channel, (double) gain);
	}
	catch (std::exception const&  ex)
	{
	}
	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_readasync(tsdrplugin_readasync_function cb, void *ctx) {
	is_running = 1;

	float * buff = NULL;
	SoapySDR::Stream *rxStream = NULL;
    int ret = 0;

	try {
		rxStream = dev_data.dev->setupStream(SOAPY_SDR_RX, dev_data.sample_format,
						dev_data.channels);
		if (rxStream == NULL) {
			/* TODO: warning */
			return 0;
		}

        size_t sample_size = SoapySDR::formatToSize(dev_data.sample_format);
        req_rate = tsdrplugin_getsamplerate();
		size_t buff_size = HOW_OFTEN_TO_CALL_CALLBACK_SEC * req_rate * 2;
		const size_t samples_per_api_read = dev_data.dev->getStreamMTU(rxStream);
		if (buff_size < samples_per_api_read * 2) buff_size = samples_per_api_read * 2;
		buff = (float *) malloc(sample_size * buff_size);
		const size_t items_per_api_read = samples_per_api_read*2;

		// initialize counters
		size_t items_in_buffer = 0;

		const uint64_t samp_rate_uint = req_rate;
		const double samp_rate_fract = req_rate - (double) samp_rate_uint;
        int flags = 0;
        long long timens = 0;
		//setup streaming
		int64_t dropped_samples = 0;
		int64_t overflows = 0;
		int64_t underflows = 0;
        size_t totalSamples = 0;

		uint64_t last_firstsample = 0;
		while (is_running) {
			if (items_per_api_read + items_in_buffer > buff_size) {
				const uint64_t samples_in_buffer = items_in_buffer >> 1;
				if (dropped_samples <= 0)
					cb(buff, items_in_buffer, ctx, 0); // nothing was dropped, nice
				else if ((dropped_samples / ((float) samples_in_buffer)) < FRACT_DROPPED_TO_TOLERATE)
					cb(buff, items_in_buffer, ctx, dropped_samples); // some part of the data was dropped, but that's fine
				else
					cb(buff, 0, ctx, dropped_samples + samples_in_buffer); // too much dropped, abort

				// reset counters, the native buffer is empty
				items_in_buffer = 0;
			}
            ret = dev_data.dev->readStream(rxStream, (void * const *) &buff[items_in_buffer], samples_per_api_read, flags, timens, 100000);
            if (ret == SOAPY_SDR_TIMEOUT) continue;
            if (ret == SOAPY_SDR_OVERFLOW)
            {
                overflows++;
                continue;
            }
            if (ret == SOAPY_SDR_UNDERFLOW)
            {
                underflows++;
                continue;
            }
            if (ret < 0)
            {
                std::cerr << "Unexpected stream error " << SoapySDR::errToStr(ret) << std::endl;
                break;
            }
            items_in_buffer += ret;
        }
	}
	catch (std::exception const&  ex)
	{
		is_running = 0;
		if (buff!=NULL) free(buff);
		RETURN_EXCEPTION(ex.what(), TSDR_CANNOT_OPEN_DEVICE);
	}
	if (buff!=NULL) free(buff);
	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API void __stdcall tsdrplugin_cleanup(void) {

	try {
		SoapySDR::Device::unmake(dev_data.dev);
	} catch (std::exception const&  ex) {

	}

	is_running = 0;
}
