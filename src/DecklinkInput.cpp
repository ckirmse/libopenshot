/**
 * @file
 * @brief Source file for DecklinkInput class
 * @author Jonathan Thomas <jonathan@openshot.org>, Blackmagic Design
 *
 * @ref License
 */

/* LICENSE
 *
 * Copyright (c) 2009 Blackmagic Design
 *
 * Permission is hereby granted, free of charge, to any person or organization
 * obtaining a copy of the software and accompanying documentation covered by
 * this license (the "Software") to use, reproduce, display, distribute,
 * execute, and transmit the Software, and to prepare derivative works of the
 * Software, and to permit third-parties to whom the Software is furnished to
 * do so, all subject to the following:
 *
 * The copyright notices in the Software and this entire statement, including
 * the above license grant, this restriction and the following disclaimer,
 * must be included in all copies of the Software, in whole or in part, and
 * all derivative works of the Software, unless such copies or derivative
 * works are solely in the form of machine-executable object code generated by
 * a source language processor.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 * FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 *
 * Copyright (c) 2008-2019 OpenShot Studios, LLC
 * <http://www.openshotstudios.com/>. This file is part of
 * OpenShot Library (libopenshot), an open-source project dedicated to
 * delivering high quality video editing and animation solutions to the
 * world. For more information visit <http://www.openshot.org/>.
 *
 * OpenShot Library (libopenshot) is free software: you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * OpenShot Library (libopenshot) is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with OpenShot Library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../include/DecklinkInput.h"

using namespace std;

DeckLinkInputDelegate::DeckLinkInputDelegate(pthread_cond_t* m_sleepCond, IDeckLinkOutput* m_deckLinkOutput, IDeckLinkVideoConversion* m_deckLinkConverter)
 : m_refCount(0), g_timecodeFormat(0), frameCount(0), final_frameCount(0)
{
	sleepCond = m_sleepCond;
	deckLinkOutput = m_deckLinkOutput;
	deckLinkConverter = m_deckLinkConverter;

	// Set cache size (20 1080p frames)
	final_frames.SetMaxBytes(60 * 1920 * 1080 * 4 + (44100 * 2 * 4));

	pthread_mutex_init(&m_mutex, NULL);
}

DeckLinkInputDelegate::~DeckLinkInputDelegate()
{
	pthread_mutex_destroy(&m_mutex);
}

ULONG DeckLinkInputDelegate::AddRef(void)
{
	pthread_mutex_lock(&m_mutex);
		m_refCount++;
	pthread_mutex_unlock(&m_mutex);

	return (ULONG)m_refCount;
}

ULONG DeckLinkInputDelegate::Release(void)
{
	pthread_mutex_lock(&m_mutex);
		m_refCount--;
	pthread_mutex_unlock(&m_mutex);

	if (m_refCount == 0)
	{
		delete this;
		return 0;
	}

	return (ULONG)m_refCount;
}

unsigned long DeckLinkInputDelegate::GetCurrentFrameNumber()
{
	if (final_frameCount > 0)
		return final_frameCount - 1;
	else
		return 0;
}

std::shared_ptr<openshot::Frame> DeckLinkInputDelegate::GetFrame(int64_t requested_frame)
{
	std::shared_ptr<openshot::Frame> f;

	// Is this frame for the future?
	while (requested_frame > GetCurrentFrameNumber())
	{
		usleep(500 * 1);
	}

	#pragma omp critical (blackmagic_input_queue)
	{
		if (final_frames.Exists(requested_frame))
		{
			// Get the frame and remove it from the cache
			f = final_frames.GetFrame(requested_frame);
			final_frames.Remove(requested_frame);
		}
		else
		{
			cout << "Can't find " << requested_frame << ", GetCurrentFrameNumber(): " << GetCurrentFrameNumber() << endl;
			final_frames.Display();
		}
	}

	return f;
}

HRESULT DeckLinkInputDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioFrame)
{
	// Handle Video Frame
	if(videoFrame)
	{	

		if (videoFrame->GetFlags() & bmdFrameHasNoInputSource)
		{
			fprintf(stderr, "Frame received (#%lu) - No input signal detected\n", frameCount);
		}
		else
		{
			const char *timecodeString = NULL;
			if (g_timecodeFormat != 0)
			{
				IDeckLinkTimecode *timecode;
				if (videoFrame->GetTimecode(g_timecodeFormat, &timecode) == S_OK)
				{
					timecode->GetString(&timecodeString);
				}
			}

//			fprintf(stderr, "Frame received (#%lu) [%s] - Size: %li bytes\n",
//				frameCount,
//				timecodeString != NULL ? timecodeString : "No timecode",
//				videoFrame->GetRowBytes() * videoFrame->GetHeight());

			if (timecodeString)
				free((void*)timecodeString);

			// Create a new copy of the YUV frame object
			IDeckLinkMutableVideoFrame *m_yuvFrame = NULL;

			int width = videoFrame->GetWidth();
			int height = videoFrame->GetHeight();

			HRESULT res = deckLinkOutput->CreateVideoFrame(
									width,
									height,
									videoFrame->GetRowBytes(),
									bmdFormat8BitYUV,
									bmdFrameFlagDefault,
									&m_yuvFrame);

			// Copy pixel and audio to copied frame
			void *frameBytesSource;
			void *frameBytesDest;
			videoFrame->GetBytes(&frameBytesSource);
			m_yuvFrame->GetBytes(&frameBytesDest);
			memcpy(frameBytesDest, frameBytesSource, videoFrame->GetRowBytes() * height);

			// Add raw YUV frame to queue
			raw_video_frames.push_back(m_yuvFrame);

			// Process frames once we have a few (to take advantage of multiple threads)
			int number_to_process = raw_video_frames.size();
			if (number_to_process >= OPEN_MP_NUM_PROCESSORS)
			{

//omp_set_num_threads(1);
omp_set_nested(true);
#pragma omp parallel
{
#pragma omp single
{
				// Temp frame counters (to keep the frames in order)
				//frameCount = 0;

				// Loop through each queued image frame
				while (!raw_video_frames.empty())
				{
					// Get front frame (from the queue)
					IDeckLinkMutableVideoFrame* frame = raw_video_frames.front();
					raw_video_frames.pop_front();

					// declare local variables (for OpenMP)
					IDeckLinkOutput *copy_deckLinkOutput(deckLinkOutput);
					IDeckLinkVideoConversion *copy_deckLinkConverter(deckLinkConverter);
					unsigned long copy_frameCount(frameCount);

					#pragma omp task firstprivate(copy_deckLinkOutput, copy_deckLinkConverter, frame, copy_frameCount)
					{
						// *********** CONVERT YUV source frame to RGB ************
						void *frameBytes;
						void *audioFrameBytes;

						// Create a new RGB frame object
						IDeckLinkMutableVideoFrame *m_rgbFrame = NULL;

						int width = videoFrame->GetWidth();
						int height = videoFrame->GetHeight();

						HRESULT res = copy_deckLinkOutput->CreateVideoFrame(
												width,
												height,
												width * 4,
												bmdFormat8BitARGB,
												bmdFrameFlagDefault,
												&m_rgbFrame);

						if(res != S_OK)
							cout << "BMDOutputDelegate::StartRunning: Error creating RGB frame, res:" << res << endl;

						// Create a RGB version of this YUV video frame
						copy_deckLinkConverter->ConvertFrame(frame, m_rgbFrame);

						// Get RGB Byte array
						m_rgbFrame->GetBytes(&frameBytes);

						// *********** CREATE OPENSHOT FRAME **********
						std::shared_ptr<openshot::Frame> f(new openshot::Frame(copy_frameCount, width, height, "#000000", 2048, 2));

						// Add Image data to openshot frame
						// TODO: Fix Decklink support with QImage Upgrade
						//f->AddImage(width, height, "ARGB", Magick::CharPixel, (uint8_t*)frameBytes);

						#pragma omp critical (blackmagic_input_queue)
						{
							// Add processed frame to cache (to be recalled in order after the thread pool is done)
							final_frames.Add(f);
						}

						// Release RGB data
						if (m_rgbFrame)
							m_rgbFrame->Release();
						// Release RGB data
						if (frame)
							frame->Release();

					} // end task

					// Increment frame count
					frameCount++;

				} // end while

} // omp single
} // omp parallel

				// Update final frameCount (since they are done processing now)
				final_frameCount += number_to_process;


			} // if size > num processors
		} // has video source
	} // if videoFrame

    return S_OK;
}

HRESULT DeckLinkInputDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags)
{
    return S_OK;
}



