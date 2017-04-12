/*
MIT License

Copyright (c) 2016-2017

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "amf-encoder.h"
#include "utility.h"
#include "components/VideoConverter.h"
#ifdef WITH_AVC
#include "components/VideoEncoderVCE.h"
#endif
#ifdef WITH_HEVC
#include "components/VideoEncoderHEVC.h"
#endif
#include <thread>
#include "libobs/util/threading.h"

using namespace Plugin;
using namespace Plugin::AMD;

Plugin::AMD::Encoder::Encoder(Codec codec,
	std::shared_ptr<API::IAPI> videoAPI, API::Adapter videoAdapter,
	bool useOpenCLSubmission, bool useOpenCLConversion,
	ColorFormat colorFormat, ColorSpace colorSpace, bool fullRangeColor,
	bool useAsyncQueue, size_t asyncQueueSize) {
	#pragma region Null Values
	m_UniqueId = Utility::GetUniqueIdentifier();
	/// AMF Internals
	m_AMF = nullptr;
	m_AMFFactory = nullptr;
	m_AMFContext = nullptr;
	m_AMFEncoder = nullptr;
	m_AMFConverter = nullptr;
	m_AMFMemoryType = amf::AMF_MEMORY_UNKNOWN;
	m_AMFSurfaceFormat = Utility::ColorFormatToAMF(colorFormat);
	/// API Related
	m_API = nullptr;
	m_APIDevice = nullptr;
	m_OpenCLSubmission = false;
	/// Properties
	m_Codec = codec;
	m_ColorFormat = colorFormat;
	m_ColorSpace = colorSpace;
	m_FullColorRange = fullRangeColor;
	m_Resolution = std::make_pair<uint32_t, uint32_t>(0, 0);
	m_FrameRate = std::make_pair<uint32_t, uint32_t>(0, 0);
	m_FrameRateTimeStep = 0;
	m_FrameRateTimeStepInt = 0;
	/// Flags
	m_Initialized = true;
	m_Started = false;
	m_OpenCL = false;
	m_OpenCLSubmission = useOpenCLSubmission;
	m_OpenCLConversion = useOpenCLConversion;
	m_HaveFirstFrame = false;
	m_AsyncQueue = useAsyncQueue;
	m_AsyncQueueSize = asyncQueueSize;
	#pragma endregion Null Values

	// Initialize selected API on Video Adapter
	m_API = videoAPI;
	m_APIAdapter = videoAdapter;
	m_APIDevice = m_API->CreateInstance(m_APIAdapter);

	// Initialize Advanced Media Framework
	m_AMF = AMF::Instance();
	/// Retrieve Factory
	m_AMFFactory = m_AMF->GetFactory();

	// Create Context for Conversion and Encoding
	AMF_RESULT res = m_AMFFactory->CreateContext(&m_AMFContext);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> Creating a AMF Context failed, error %ls (code %d).",
			m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
		throw std::exception(errMsg.c_str());
	}
	/// Initialize Context using selected API
	switch (m_API->GetType()) {
		case API::Type::Direct3D11:
		case API::Type::Direct3D9:
			break;
		default:
			m_API = API::GetAPI(0);
			switch (m_API->GetType()) {
				case API::Type::Direct3D9:
					m_APIAdapter = m_API->EnumerateAdapters()[0];
					m_APIDevice = m_API->CreateInstance(m_APIAdapter);
					break;
				case API::Type::Direct3D11:
					m_APIAdapter = m_API->EnumerateAdapters()[0];
					m_APIDevice = m_API->CreateInstance(m_APIAdapter);
					break;
			}
	}
	switch (m_API->GetType()) {
		case API::Type::Direct3D9:
			m_AMFMemoryType = amf::AMF_MEMORY_DX9;
			res = m_AMFContext->InitDX9(m_APIDevice->GetContext());
			break;
		case API::Type::Direct3D11:
			m_AMFMemoryType = amf::AMF_MEMORY_DX11;
			res = m_AMFContext->InitDX11(m_APIDevice->GetContext());
			break;
	}
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> Initializing %s API with Adapter '%s' failed, error %ls (code %d).",
			m_UniqueId,
			m_API->GetName().c_str(), m_APIAdapter.Name.c_str(),
			m_AMF->GetTrace()->GetResultText(res), res);
		throw std::exception(errMsg.c_str());
	}

	// Initialize OpenCL (if possible)
	if (m_OpenCLSubmission || m_OpenCLConversion) {
		res = m_AMFContext->InitOpenCL();
		if (res == AMF_OK) {
			m_OpenCL = true;

			res = m_AMFContext->GetCompute(amf::AMF_MEMORY_OPENCL, &m_AMFCompute);
			if (res != AMF_OK) {
				m_OpenCLSubmission = false;
				m_OpenCLConversion = false;

				QUICK_FORMAT_MESSAGE(errMsg,
					"<Id: %lld> Retrieving Compute object failed, error %ls (code %d)",
					m_UniqueId,
					m_AMF->GetTrace()->GetResultText(res), res);
				PLOG_WARNING("%s", errMsg.data());
			}
		} else {
			m_OpenCL = false;
			m_OpenCLSubmission = false;
			m_OpenCLConversion = false;

			QUICK_FORMAT_MESSAGE(errMsg,
				"<Id: %lld> Initialising OpenCL failed, error %ls (code %d)",
				m_UniqueId,
				m_AMF->GetTrace()->GetResultText(res), res);
			PLOG_WARNING("%s", errMsg.data());
		}
	}

	// Create Converter
	res = m_AMFFactory->CreateComponent(m_AMFContext, AMFVideoConverter, &m_AMFConverter);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> Creating frame converter component failed, error %ls (code %d)",
			m_UniqueId,
			m_AMF->GetTrace()->GetResultText(res), res);
		throw std::exception(errMsg.c_str());
	}
	res = m_AMFConverter->SetProperty(AMF_VIDEO_CONVERTER_MEMORY_TYPE, amf::AMF_MEMORY_UNKNOWN);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> Unable to set converter memory type, error %ls (code %d)",
			m_UniqueId,
			m_AMF->GetTrace()->GetResultText(res), res);
		throw std::exception(errMsg.c_str());
	}
	res = m_AMFConverter->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, amf::AMF_SURFACE_NV12);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> Unable to set converter output format, error %ls (code %d)",
			m_UniqueId,
			m_AMF->GetTrace()->GetResultText(res), res);
		throw std::exception(errMsg.c_str());
	}
	res = m_AMFConverter->SetProperty(AMF_VIDEO_CONVERTER_COLOR_PROFILE, Utility::ColorSpaceToAMFConverter(m_ColorSpace));
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> Unable to set convertor color profile, error %ls (code %d)",
			m_UniqueId,
			m_AMF->GetTrace()->GetResultText(res), res);
		throw std::exception(errMsg.c_str());
	}

	// Create Encoder
	res = m_AMFFactory->CreateComponent(m_AMFContext, Utility::CodecToAMF(codec), &m_AMFEncoder);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> Unable to create %s encoder, error %ls (code %d)",
			m_UniqueId,
			Utility::CodecToString(codec),
			m_AMF->GetTrace()->GetResultText(res),
			res);
		throw std::exception(errMsg.c_str());
	}

	// Show complete initialization in log.
	QUICK_FORMAT_MESSAGE(notice,
		"<Id: %lld> Initialized.",
		m_UniqueId);
	PLOG_DEBUG("%s", notice.data());
}

Plugin::AMD::Encoder::~Encoder() {
	// Destroy AMF Encoder
	if (m_AMFEncoder) {
		m_AMFEncoder->Terminate();
		m_AMFEncoder = nullptr;
	}

	// Destroy AMF Converter
	if (m_AMFConverter) {
		m_AMFConverter->Terminate();
		m_AMFConverter = nullptr;
	}

	// Destroy AMF Context
	if (m_AMFContext) {
		m_AMFContext->Terminate();
		m_AMFContext = nullptr;
	}

	// Destroy API
	if (m_API) {
		m_APIDevice = nullptr;
		m_API = nullptr;
	}

	m_AMF = nullptr;

	// Show complete initialization in log.
	QUICK_FORMAT_MESSAGE(notice,
		"<Id: %lld> Finalized.",
		m_UniqueId);
	PLOG_DEBUG("%s", notice.c_str());
}

uint64_t Plugin::AMD::Encoder::GetUniqueId() {
	return m_UniqueId;
}

Plugin::AMD::Codec Plugin::AMD::Encoder::GetCodec() {
	return m_Codec;
}

std::shared_ptr<API::IAPI> Plugin::AMD::Encoder::GetVideoAPI() {
	return m_API;
}

Plugin::API::Adapter Plugin::AMD::Encoder::GetVideoAdapter() {
	return m_APIAdapter;
}

bool Plugin::AMD::Encoder::IsOpenCLEnabled() {
	return m_OpenCLSubmission;
}

Plugin::AMD::ColorFormat Plugin::AMD::Encoder::GetColorFormat() {
	return m_ColorFormat;
}

Plugin::AMD::ColorSpace Plugin::AMD::Encoder::GetColorSpace() {
	return m_ColorSpace;
}

bool Plugin::AMD::Encoder::IsFullRangeColor() {
	return m_FullColorRange;
}

void Plugin::AMD::Encoder::UpdateFrameRateValues() {
	// 1			Second
	// 1000			Millisecond
	// 1000000		Microsecond
	// 10000000		amf_pts
	// 1000000000	Nanosecond
	m_FrameRateFraction = ((double_t)m_FrameRate.second / (double_t)m_FrameRate.first);
	m_FrameRateTimeStep = AMF_SECOND * m_FrameRateFraction;
	m_FrameRateTimeStepInt = (uint64_t)round(m_FrameRateTimeStep);
	m_SubmitQueryWaitTimer = std::chrono::nanoseconds((uint64_t)round(m_FrameRateTimeStep / m_SubmitQueryAttempts / 2));
}

void Plugin::AMD::Encoder::SetVBVBufferStrictness(double_t v) {
	AMFTRACECALL;

	auto bitrateCaps = CapsVBVBufferSize();
	uint64_t looseBitrate = bitrateCaps.second,
		targetBitrate = 0,
		strictBitrate = bitrateCaps.first;

	Usage usage = GetUsage();
	if (usage == Usage::UltraLowLatency) {
		targetBitrate = clamp(GetTargetBitrate(), bitrateCaps.first, bitrateCaps.second);
	} else {
		switch (this->GetRateControlMethod()) {
			case RateControlMethod::ConstantBitrate:
				targetBitrate = clamp(GetTargetBitrate(), bitrateCaps.first, bitrateCaps.second);
				break;
			case RateControlMethod::LatencyConstrainedVariableBitrate:
			case RateControlMethod::PeakConstrainedVariableBitrate:
				targetBitrate = max(this->GetTargetBitrate(), this->GetPeakBitrate());
				break;
			case RateControlMethod::ConstantQP:
				targetBitrate = bitrateCaps.second / 2;
				break;
		}
	}
	strictBitrate = clamp(static_cast<uint64_t>(
		round(targetBitrate * ((double_t)m_FrameRate.second / (double_t)m_FrameRate.first))
		), bitrateCaps.first, targetBitrate);

	// Three-Point Linear Lerp
	// 0% = looseBitrate, 50% = targetBitrate, 100% = strictBitrate
	v = clamp(v, 0.0, 1.0);
	double_t aFadeVal = clamp(v * 2.0, 0.0, 1.0); // 0 - 0.5
	double_t bFadeVal = clamp(v * 2.0 - 1.0, 0.0, 0.0); // 0.5 - 1.0

	double_t aFade = (looseBitrate * (1.0 - aFadeVal)) + (targetBitrate * aFadeVal);
	double_t bFade = (aFade * (1.0 - bFadeVal)) + (strictBitrate * bFadeVal);

	uint64_t vbvBufferSize = static_cast<uint64_t>(round(bFade));
	this->SetVBVBufferSize(vbvBufferSize);
}

void Plugin::AMD::Encoder::Start() {
	AMFTRACECALL;

	AMF_RESULT res;

	res = m_AMFConverter->Init(Utility::ColorFormatToAMF(m_ColorFormat), m_Resolution.first, m_Resolution.second);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> Unable to initalize converter, error %ls (code %d)",
			m_UniqueId,
			m_AMF->GetTrace()->GetResultText(res),
			res);
		throw std::exception(errMsg.c_str());
	}

	res = m_AMFEncoder->Init(amf::AMF_SURFACE_NV12, m_Resolution.first, m_Resolution.second);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> Failed to initialize encoder, error %ls (code %d)",
			m_UniqueId,
			m_AMF->GetTrace()->GetResultText(res), res);
		throw std::exception(errMsg.c_str());
	}

	// Threading
	if (m_AsyncQueue) {
		m_AsyncSend = new EncoderThreadingData;
		m_AsyncSend->shutdown = false;
		m_AsyncSend->wakeupcount = 0;// 2 ^ 32;
		m_AsyncSend->worker = std::thread(AsyncSendMain, this);
		m_AsyncRetrieve = new EncoderThreadingData;
		m_AsyncRetrieve->shutdown = false;
		m_AsyncRetrieve->wakeupcount = 0;
		m_AsyncRetrieve->worker = std::thread(AsyncRetrieveMain, this);
	}

	m_Started = true;
}

void Plugin::AMD::Encoder::Restart() {
	AMFTRACECALL;

	AMF_RESULT res = m_AMFEncoder->ReInit(m_Resolution.first, m_Resolution.second);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> Could not re-initialize encoder, error %ls (code %d)",
			m_UniqueId,
			m_AMF->GetTrace()->GetResultText(res), res);
		throw std::exception(errMsg.c_str());
	}
}

void Plugin::AMD::Encoder::Stop() {
	AMFTRACECALL;

	if (!m_Started)
		throw std::logic_error("Can't stop an encoder that isn't running!");

	m_AMFConverter->Drain();
	m_AMFConverter->Flush();
	m_AMFEncoder->Drain();
	m_AMFEncoder->Flush();

	// Threading
	if (m_AsyncQueue) {
		{
			std::unique_lock<std::mutex> lock(m_AsyncRetrieve->mutex);
			m_AsyncRetrieve->shutdown = true;
			m_AsyncRetrieve->wakeupcount = 2 ^ 32;
			m_AsyncRetrieve->condvar.notify_all();
		}
		m_AsyncRetrieve->worker.join();
		delete m_AsyncRetrieve;
		{
			std::unique_lock<std::mutex> lock(m_AsyncSend->mutex);
			m_AsyncSend->shutdown = true;
			m_AsyncSend->wakeupcount = 2 ^ 32;
			m_AsyncSend->condvar.notify_all();
		}
		m_AsyncSend->worker.join();
		delete m_AsyncSend;
	}

	m_Started = false;
}

bool Plugin::AMD::Encoder::IsStarted() {
	AMFTRACECALL;

	return m_Started;
}

bool Plugin::AMD::Encoder::Encode(struct encoder_frame* frame, struct encoder_packet* packet, bool* received_packet) {
	AMFTRACECALL;

	if (!m_Started)
		return false;

	amf::AMFSurfacePtr surface = nullptr;
	amf::AMFDataPtr surface_data = nullptr;
	amf::AMFDataPtr packet_data = nullptr;

	// Encoding Steps
	if (!EncodeAllocate(surface))
		return false;
	if (!EncodeStore(surface, frame))
		return false;
	if (!EncodeConvert(surface, surface_data))
		return false;
	if (!EncodeMain(surface_data, packet_data))
		return false;
	if (!EncodeLoad(packet_data, packet, received_packet))
		return false;

	return true;
}

void Plugin::AMD::Encoder::GetVideoInfo(struct video_scale_info* info) {
	AMFTRACECALL;

	if (!m_AMFContext || !m_AMFEncoder)
		throw std::exception("<" __FUNCTION_NAME__ "> Called while not initialized.");

	switch (m_ColorFormat) {
		// 4:2:0 Formats
		case ColorFormat::NV12:
			info->format = VIDEO_FORMAT_NV12;
			break;
		case ColorFormat::I420:
			info->format = VIDEO_FORMAT_I420;
			break;
			// 4:2:2 Formats
		case ColorFormat::YUY2:
			info->format = VIDEO_FORMAT_YUY2;
			break;
			// Uncompressed
		case ColorFormat::RGBA:
			info->format = VIDEO_FORMAT_RGBA;
			break;
		case ColorFormat::BGRA:
			info->format = VIDEO_FORMAT_BGRA;
			break;
			// Other
		case ColorFormat::GRAY:
			info->format = VIDEO_FORMAT_Y800;
			break;
	}

	if (m_FullColorRange) { // Only use Full range if actually enabled.
		info->range = VIDEO_RANGE_FULL;
	} else {
		info->range = VIDEO_RANGE_PARTIAL;
	}
}

bool Plugin::AMD::Encoder::GetExtraData(uint8_t** extra_data, size_t* size) {
	AMFTRACECALL;

	if (!m_AMFContext || !m_AMFEncoder)
		throw std::exception("<" __FUNCTION_NAME__ "> Called while not initialized.");

	amf::AMFVariant var;
	AMF_RESULT res = GetExtraDataInternal(&var);
	if (res == AMF_OK && var.type == amf::AMF_VARIANT_INTERFACE) {
		amf::AMFBufferPtr buf(var.pInterface);

		*size = buf->GetSize();
		m_ExtraDataBuffer.resize(*size);
		std::memcpy(m_ExtraDataBuffer.data(), buf->GetNative(), *size);
		*extra_data = m_ExtraDataBuffer.data();

		return true;
	}
	return false;
}

bool Plugin::AMD::Encoder::EncodeAllocate(OUT amf::AMFSurfacePtr& surface) {
	AMFTRACECALL;

	AMF_RESULT res;
	auto clk_start = std::chrono::high_resolution_clock::now();

	// Allocate
	if (m_OpenCLSubmission) {
		res = m_AMFContext->AllocSurface(m_AMFMemoryType, m_AMFSurfaceFormat,
			m_Resolution.first, m_Resolution.second, &surface);
	} else {
		// Required when not using OpenCL, can't directly write to GPU memory with memcpy.
		res = m_AMFContext->AllocSurface(amf::AMF_MEMORY_HOST, m_AMFSurfaceFormat,
			m_Resolution.first, m_Resolution.second, &surface);
	}
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> Unable to allocate Surface, error %ls (code %d)",
			m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
		PLOG_ERROR("%s", errMsg.data());
		return false;
	}

	// Performance Tracking
	auto clk_end = std::chrono::high_resolution_clock::now();
	uint64_t pf_timestamp = std::chrono::nanoseconds(clk_end.time_since_epoch()).count();
	uint64_t pf_time = std::chrono::nanoseconds(clk_end - clk_start).count();

	surface->SetProperty(AMF_TIMESTAMP_ALLOCATE, pf_timestamp);
	surface->SetProperty(AMF_TIME_ALLOCATE, pf_time);

	return true;
}

bool Plugin::AMD::Encoder::EncodeStore(OUT amf::AMFSurfacePtr& surface, IN struct encoder_frame* frame) {
	AMFTRACECALL;

	AMF_RESULT res;
	amf::AMFComputeSyncPointPtr pSyncPoint;
	auto clk_start = std::chrono::high_resolution_clock::now();

	if (m_OpenCLSubmission) {
		m_AMFCompute->PutSyncPoint(&pSyncPoint);
		res = surface->Convert(amf::AMF_MEMORY_OPENCL);
		if (res != AMF_OK) {
			QUICK_FORMAT_MESSAGE(errMsg,
				"<Id: %lld> [Store] Conversion of Surface to OpenCL failed, error %ls (code %d)",
				m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
			PLOG_WARNING("%s", errMsg.data());
			return false;
		}
	}

	size_t planeCount = surface->GetPlanesCount();
	for (uint8_t i = 0; i < planeCount; i++) {
		amf::AMFPlanePtr plane = surface->GetPlaneAt(i);
		int32_t width = plane->GetWidth();
		int32_t height = plane->GetHeight();
		int32_t hpitch = plane->GetHPitch();

		if (m_OpenCLSubmission) {
			static const amf_size l_origin[] = { 0, 0, 0 };
			const amf_size l_size[] = { (amf_size)width, (amf_size)height, 1 };
			res = m_AMFCompute->CopyPlaneFromHost(frame->data[i], l_origin, l_size, frame->linesize[i], surface->GetPlaneAt(i), false);
			if (res != AMF_OK) {
				QUICK_FORMAT_MESSAGE(errMsg,
					"<Id: %lld> [Store] Unable to copy plane %d with OpenCL, error %ls (code %d)",
					m_UniqueId, i, m_AMF->GetTrace()->GetResultText(res), res);
				PLOG_WARNING("%s", errMsg.data());
				return false;
			}
		} else {
			void* plane_nat = plane->GetNative();
			for (int32_t py = 0; py < height; py++) {
				int32_t plane_off = py * hpitch;
				int32_t frame_off = py * frame->linesize[i];
				std::memcpy(
					static_cast<void*>(static_cast<uint8_t*>(plane_nat) + plane_off),
					static_cast<void*>(frame->data[i] + frame_off), frame->linesize[i]);
			}
		}
	}

	if (m_OpenCLSubmission) {
		res = m_AMFCompute->FinishQueue();
		if (res != AMF_OK) {
			QUICK_FORMAT_MESSAGE(errMsg,
				"<Id: %lld> [Store] Failed to finish OpenCL queue, error %ls (code %d)",
				m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
			PLOG_WARNING("%s", errMsg.data());
			return false;
		}
		pSyncPoint->Wait();
	}
	res = surface->Convert(m_AMFMemoryType);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> [Store] Conversion of Surface failed, error %ls (code %d)",
			m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
		PLOG_WARNING("%s", errMsg.data());
		return false;
	}

	// Data Stuff
	int64_t tsLast = (int64_t)round((frame->pts - 1) * m_FrameRateTimeStep);
	int64_t tsNow = (int64_t)round(frame->pts * m_FrameRateTimeStep);

	/// Decode Timestamp
	surface->SetPts(tsNow);
	/// Presentation Timestamp
	surface->SetProperty(AMF_PRESENT_TIMESTAMP, frame->pts);
	/// Duration
	surface->SetDuration(tsNow - tsLast);

	// Performance Tracking
	auto clk_end = std::chrono::high_resolution_clock::now();
	uint64_t pf_timestamp = std::chrono::nanoseconds(clk_end.time_since_epoch()).count();
	uint64_t pf_time = std::chrono::nanoseconds(clk_end - clk_start).count();
	surface->SetProperty(AMF_TIMESTAMP_STORE, pf_timestamp);
	surface->SetProperty(AMF_TIME_STORE, pf_time);

	PLOG_DEBUG("EncodeStore: PTS(%8lld) DTS(%8lld) TS(%16lld) Duration(%16lld)",
		frame->pts,
		frame->pts,
		surface->GetPts(),
		surface->GetDuration());

	return true;
}

bool Plugin::AMD::Encoder::EncodeConvert(IN amf::AMFSurfacePtr& surface, OUT amf::AMFDataPtr& data) {
	AMFTRACECALL;

	AMF_RESULT res;
	auto clk_start = std::chrono::high_resolution_clock::now();

	if (m_OpenCLConversion) {
		res = surface->Convert(amf::AMF_MEMORY_OPENCL);
		if (res != AMF_OK) {
			QUICK_FORMAT_MESSAGE(errMsg,
				"<Id: %lld> [Convert] Conversion of Surface to OpenCL failed, error %ls (code %d)",
				m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
			PLOG_WARNING("%s", errMsg.data());
			return false;
		}
	}
	res = m_AMFConverter->SubmitInput(surface);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> [Convert] Submit to converter failed, error %ls (code %d)",
			m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
		PLOG_WARNING("%s", errMsg.data());
		return false;
	}
	res = m_AMFConverter->QueryOutput(&data);
	if (res != AMF_OK) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> [Convert] Querying output from converter failed, error %ls (code %d)",
			m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
		PLOG_WARNING("%s", errMsg.data());
		return false;
	}
	if (m_OpenCLConversion) {
		res = surface->Convert(m_AMFMemoryType);
		if (res != AMF_OK) {
			QUICK_FORMAT_MESSAGE(errMsg,
				"<Id: %lld> [Convert] Conversion of Surface failed, error %ls (code %d)",
				m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
			PLOG_WARNING("%s", errMsg.data());
			return false;
		}
	}

	// Performance Tracking
	auto clk_end = std::chrono::high_resolution_clock::now();
	uint64_t pf_timestamp = std::chrono::nanoseconds(clk_end.time_since_epoch()).count();
	uint64_t pf_time = std::chrono::nanoseconds(clk_end - clk_start).count();
	surface->SetProperty(AMF_TIMESTAMP_CONVERT, pf_timestamp);
	surface->SetProperty(AMF_TIME_CONVERT, pf_time);

	return true;
}

bool Plugin::AMD::Encoder::EncodeMain(IN amf::AMFDataPtr& data, OUT amf::AMFDataPtr& packet) {
	AMFTRACECALL;

	bool frameSubmitted = false,
		packetRetrieved = false;

	for (uint64_t attempt = 1; 
		((attempt <= m_SubmitQueryAttempts) && (!frameSubmitted || !m_HaveFirstFrame)) 
		|| (m_HaveFirstFrame && !packetRetrieved);
		attempt++) {
		// Submit
		if (!frameSubmitted) {
			if (m_AsyncQueue) { // Asynchronous
				std::unique_lock<std::mutex> slock(m_AsyncSend->mutex);
				if (m_AsyncSend->queue.size() < m_AsyncQueueSize) {
					m_AsyncSend->queue.push(data);
					m_AsyncSend->wakeupcount++;
					m_AsyncSend->condvar.notify_one();
					frameSubmitted = true;
				} else {
					m_AsyncSend->wakeupcount++;
					m_AsyncSend->condvar.notify_one();
				}
			} else {
				// Performance Tracking
				auto clk = std::chrono::high_resolution_clock::now();
				uint64_t pf_ts = std::chrono::nanoseconds(clk.time_since_epoch()).count();
				data->SetProperty(AMF_TIMESTAMP_SUBMIT, pf_ts);

				AMF_RESULT res = m_AMFEncoder->SubmitInput(data);
				switch (res) {
					case AMF_INPUT_FULL: // TODO: We don't really have a way to call QueryOutput here...
						break;
					case AMF_OK:
						frameSubmitted = true;
						break;
					default:
						{
							QUICK_FORMAT_MESSAGE(errMsg,
								"<Id: %lld> [Main] Submitting Surface failed, error %ls (code %d)",
								m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
							PLOG_ERROR("%s", errMsg.data());
						}
						return false;
				}
			}
		}

		if (!frameSubmitted)
			std::this_thread::sleep_for(m_SubmitQueryWaitTimer);

		// Retrieve
		if (!packetRetrieved) {
			if (m_AsyncQueue) {
				std::unique_lock<std::mutex> rlock(m_AsyncRetrieve->mutex);
				if (m_AsyncRetrieve->queue.size() > 0) {
					packet = m_AsyncRetrieve->queue.front();
					m_AsyncRetrieve->queue.pop();
					packetRetrieved = true;
					m_HaveFirstFrame = true;
				} else {
					//m_AsyncRetrieve->wakeupcount++;
					m_AsyncRetrieve->condvar.notify_one();
				}
			} else {
				AMF_RESULT res = m_AMFEncoder->QueryOutput(&packet);
				switch (res) {
					case AMF_REPEAT: // Returned with B-Frames, means that we need more frames.
					case AMF_NEED_MORE_INPUT: // Same
						if (!m_HaveFirstFrame)
							packetRetrieved = true;
						// TODO: Somehow call SubmitInput here.
						break;
					case AMF_OK:
						m_HaveFirstFrame = true;
						packetRetrieved = true;

						// Performance Tracking
						{
							auto clk = std::chrono::high_resolution_clock::now();
							uint64_t pf_query = std::chrono::nanoseconds(clk.time_since_epoch()).count(),
								pf_submit, pf_main;
							packet->GetProperty(AMF_TIMESTAMP_SUBMIT, &pf_submit);
							packet->SetProperty(AMF_TIMESTAMP_QUERY, pf_query);
							pf_main = (pf_query - pf_submit);
							packet->SetProperty(AMF_TIME_MAIN, pf_main);
						}

						break;
					default:
						{
							QUICK_FORMAT_MESSAGE(errMsg,
								"<Id: %lld> [Main] Retrieving Packet failed, error %ls (code %d)",
								m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
							PLOG_ERROR("%s", errMsg.data());
						}
						return false;
				}
			}
		}

		if (!packetRetrieved)
			std::this_thread::sleep_for(m_SubmitQueryWaitTimer);
	}
	if (!frameSubmitted) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> Input Queue is full, encoder is overloaded!",
			m_UniqueId);
		PLOG_WARNING("%s", errMsg.data());
	}
	if (m_HaveFirstFrame && !packetRetrieved) {
		QUICK_FORMAT_MESSAGE(errMsg,
			"<Id: %lld> No output Packet, encoder is overloaded!",
			m_UniqueId);
		PLOG_WARNING("%s", errMsg.data());
	}
	return true;
}

bool Plugin::AMD::Encoder::EncodeLoad(IN amf::AMFDataPtr& data, OUT struct encoder_packet* packet, OUT bool* received_packet) {
	AMFTRACECALL;

	if (data == nullptr)
		return true;

	amf::AMFBufferPtr pBuffer = amf::AMFBufferPtr(data);
	auto clk_start = std::chrono::high_resolution_clock::now();

	// Timestamps
	packet->type = OBS_ENCODER_VIDEO;
	/// Present Timestamp
	data->GetProperty(AMF_PRESENT_TIMESTAMP, &packet->pts);
	/// Decode Timestamp
	packet->dts = (int64_t)round((double_t)data->GetPts() / m_FrameRateTimeStep);
	/// Data
	PacketPriorityAndKeyframe(data, packet);
	packet->size = pBuffer->GetSize();
	if (m_PacketDataBuffer.size() < packet->size) {
		size_t newBufferSize = (size_t)exp2(ceil(log2(packet->size)));
		//AMF_LOG_DEBUG("Packet Buffer was resized to %d byte from %d byte.", newBufferSize, m_PacketDataBuffer.size());
		m_PacketDataBuffer.resize(newBufferSize);
	}
	packet->data = m_PacketDataBuffer.data();
	std::memcpy(packet->data, pBuffer->GetNative(), packet->size);

	// Performance Tracking
	auto clk_end = std::chrono::high_resolution_clock::now();
	uint64_t pf_allocate_ts, pf_allocate_t,
		pf_store_ts, pf_store_t,
		pf_convert_ts, pf_convert_t,
		pf_submit_ts, pf_query_ts, pf_main_t,
		pf_load_ts, pf_load_t;

	data->GetProperty(AMF_TIMESTAMP_ALLOCATE, &pf_allocate_ts);
	data->GetProperty(AMF_TIME_ALLOCATE, &pf_allocate_t);
	data->GetProperty(AMF_TIMESTAMP_STORE, &pf_store_ts);
	data->GetProperty(AMF_TIME_STORE, &pf_store_t);
	data->GetProperty(AMF_TIMESTAMP_CONVERT, &pf_convert_ts);
	data->GetProperty(AMF_TIME_CONVERT, &pf_convert_t);
	data->GetProperty(AMF_TIMESTAMP_SUBMIT, &pf_submit_ts);
	data->GetProperty(AMF_TIMESTAMP_QUERY, &pf_query_ts);
	data->GetProperty(AMF_TIME_MAIN, &pf_main_t);
	pf_load_ts = std::chrono::nanoseconds(clk_end.time_since_epoch()).count();
	pf_load_t = std::chrono::nanoseconds(clk_end - clk_start).count();

	PLOG_DEBUG(
		" EncodeLoad: PTS(%8lld) DTS(%8lld) TS(%16lld) Duration(%16lld) Size(%16lld)",
		packet->pts,
		packet->dts,
		data->GetPts(),
		data->GetDuration(),
		packet->size);
	PLOG_DEBUG("   Timings: Allocate(%8lld ns) Store(%8lld ns) Convert(%8lld ns) Main(%8lld ns) Load(%8lld ns)",
		pf_allocate_t,
		pf_store_t,
		pf_convert_t,
		pf_main_t,
		pf_load_t);

	*received_packet = true;

	return true;
}

int32_t Plugin::AMD::Encoder::AsyncSendMain(Encoder* obj) {
	os_set_thread_name("AMF Asynchronous Queue Sender");
	return obj->AsyncSendLocalMain();
}

int32_t Plugin::AMD::Encoder::AsyncSendLocalMain() {
	EncoderThreadingData* own = m_AsyncSend;

	std::unique_lock<std::mutex> lock(own->mutex);
	while (!own->shutdown) {
		own->condvar.wait(lock, [&own] {
			return own->shutdown || !own->queue.empty();
		});

		if (own->queue.empty())
			continue;

		AMF_RESULT res = m_AMFEncoder->SubmitInput(own->queue.front());
		switch (res) {
			case AMF_OK:
				own->queue.pop();
				own->wakeupcount--;
				// No break since the behaviour is identical here.
			case AMF_INPUT_FULL:
				{
					std::unique_lock<std::mutex> rlock(m_AsyncRetrieve->mutex);
					m_AsyncRetrieve->wakeupcount++;
					m_AsyncRetrieve->condvar.notify_one();
				}
				break;
			default:
				{
					QUICK_FORMAT_MESSAGE(errMsg,
						"<Id: %lld> Submitting Surface failed, error %ls (code %d)",
						m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
					PLOG_ERROR("%s", errMsg.data());
				}
				return -1;
		}

		std::this_thread::sleep_for(m_SubmitQueryWaitTimer);
	}
	return 0;
}

int32_t Plugin::AMD::Encoder::AsyncRetrieveMain(Encoder* obj) {
	os_set_thread_name("AMF Asynchronous Queue Retriever");
	return obj->AsyncRetrieveLocalMain();
}

int32_t Plugin::AMD::Encoder::AsyncRetrieveLocalMain() {
	EncoderThreadingData* own = m_AsyncRetrieve;

	std::unique_lock<std::mutex> lock(own->mutex);
	while (!own->shutdown) {
		own->condvar.wait(lock, [&own] {
			return own->shutdown || (own->wakeupcount > 0);
		});

		if (own->wakeupcount == 0)
			continue;

		if (own->queue.size() < m_AsyncQueueSize) {
			amf::AMFDataPtr packet;
			AMF_RESULT res = m_AMFEncoder->QueryOutput(&packet);
			switch (res) {
				case AMF_NEED_MORE_INPUT:
				case AMF_REPEAT:
					{
						std::unique_lock<std::mutex> slock(m_AsyncSend->mutex);
						if (!m_AsyncSend->queue.empty())
							m_AsyncSend->condvar.notify_one();
					}
					break;
				case AMF_OK:
					own->queue.push(packet);
					own->wakeupcount--;

					// Performance Tracking
					{
						auto clk = std::chrono::high_resolution_clock::now();
						uint64_t pf_query = std::chrono::nanoseconds(clk.time_since_epoch()).count(),
							pf_submit, pf_main;
						packet->GetProperty(AMF_TIMESTAMP_SUBMIT, &pf_submit);
						packet->SetProperty(AMF_TIMESTAMP_QUERY, pf_query);
						pf_main = (pf_query - pf_submit);
						packet->SetProperty(AMF_TIME_MAIN, pf_main);
					}
					break;
				default:
					{
						QUICK_FORMAT_MESSAGE(errMsg,
							"<Id: %lld> Retrieving Packet failed, error %ls (code %d)",
							m_UniqueId, m_AMF->GetTrace()->GetResultText(res), res);
						PLOG_ERROR("%s", errMsg.data());
					}
					return -1;
			}
		}

		std::this_thread::sleep_for(m_SubmitQueryWaitTimer);
	}
	return 0;
}
