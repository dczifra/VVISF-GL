#include "AudioController.h"

#include <QDebug>
#include <QTime>
#include <algorithm>




static AudioController * globalAudioController = nullptr;
//static int					fftQuality = 512;




AudioController::AudioController(QObject *parent) :
	QObject(parent),
	//m_input(fftQuality*2, 0.0),
	m_output(fftQuality*2, 0.0)
{
	qDebug() << __PRETTY_FUNCTION__;
	
	_audioUploader = CreateGLCPUToTexCopierRef();
	_audioUploader->setQueueSize(1);
	
	_fftUploader = CreateGLCPUToTexCopierRef();
	_fftUploader->setQueueSize(1);
	
	_device.reset(new QAudioDeviceInfo(QAudioDeviceInfo::defaultInputDevice()));
	
	_format.setSampleRate(48000);
	_format.setChannelCount(1);
	//_format.setSampleSize(16);
	//_format.setSampleType(QAudioFormat::SignedInt);
	_format.setSampleSize(32);
	_format.setSampleType(QAudioFormat::Float);
	_format.setByteOrder(QAudioFormat::LittleEndian);
	_format.setCodec("audio/pcm");
	if (!_device->isFormatSupported(_format))	{
		qDebug() << "ERR: default format not supported, going with nearest";
		_format = _device->nearestFormat(_format);
	}
	
	_input.reset(new QAudioInput(*_device, _format));
	qreal			linearVolume = QAudio::convertVolume(100.0, QAudio::LogarithmicVolumeScale, QAudio::LinearVolumeScale);
	_input->setVolume(linearVolume);
	auto			inputDevice = _input->start();
	if (inputDevice != nullptr)	{
		connect(inputDevice, &QIODevice::readyRead, [&,inputDevice]()	{
			//qDebug() << QTime::currentTime();
			
			//	lock, we're going to be modifying the buffer list
			std::lock_guard<recursive_mutex>		tmpLock(_lock);
			
			//	dump all the available samples to a byte array
			QByteArray			rawByteArray = inputDevice->readAll();
			qint64				totalBytesAvailable = rawByteArray.size();
			int					bytesPerSample = _format.sampleSize() / 8;
			int					numSamplesAvailable = int(totalBytesAvailable / bytesPerSample);
			int					samplesCopiedSoFar = 0;
			//	split up the byte array into smaller byte arrays, make IFSAudioBufferList instances from them and dump them to _bufferList
			//qDebug() << "\t\t_bufferList had " << _bufferList.size() << " items";
			while ((samplesCopiedSoFar + fftQuality) <= numSamplesAvailable)	{
				QByteArray			subArray = rawByteArray.mid(samplesCopiedSoFar * bytesPerSample, fftQuality * bytesPerSample);
				//qDebug() << "\t\tadding ABL with size " << subArray.size() << " to _bufferList...";
				_bufferList.append( ISFAudioBufferList(subArray, _format) );
				samplesCopiedSoFar += fftQuality;
			}
			
			//	make sure _bufferList isn't growing too large
			while (_bufferList.size() > 12)	{
				_bufferList.removeFirst();
			}
			//qDebug() << "\t\t_bufferList now has " << _bufferList.size() << " items";
			
		});
	}
}

QList<QAudioDeviceInfo> AudioController::getListOfDevices()	{
	return QAudioDeviceInfo::availableDevices(QAudio::AudioInput);
}
QAudioDeviceInfo * AudioController::currentDeviceInfo()	{
	if (_device.isNull())
		return nullptr;
	return _device.data();
}

static float			tmpMax = 0.0;
static float			tmpMin = 99999999.0;
void AudioController::updateAudioResults()	{
	
	QLinkedList<ISFAudioBufferList>		ablsToProcess;
	{
		//	run through and make sure there are enough samples to create an ISFAudioBufferList twice the size of fftQuality
		std::lock_guard<recursive_mutex>		tmpLock(_lock);
		int			samplesAvailable = 0;
		for (const ISFAudioBufferList & tmpBuffer : _bufferList)	{
			samplesAvailable += tmpBuffer.numberOfFrames;
			ablsToProcess.append(tmpBuffer);
			if (samplesAvailable >= (fftQuality * 2))
				break;
		}
		if (samplesAvailable < (fftQuality * 2))
			return;
		//qDebug() << "ablsToProcess is " << ablsToProcess.size();
		//qDebug() << "removing " << (ablsToProcess.size()-1) << " items from _bufferList...";
		for (int i=0; i<(ablsToProcess.size()-1); ++i)	{
			_bufferList.removeFirst();
		}
		//qDebug() << "_bufferList now has " << _bufferList.size() << " items";
	}
	
	//	make a single ISFAudioBufferList by combining two- this is what we're going to be working with
	ISFAudioBufferList		ablToProcess(ablsToProcess);
	
	//	run the FFT on the large audio buffer list
	if (ablToProcess.numberOfFrames == fftQuality*2)	{
		//	calculate the fft, dump it to m_output
		m_fft.calculateFFT(m_output.data(), ablToProcess.floatPtr());
		
		//	...from here on out, i'm not sure if i'm doing this correctly- the results are not an exact match to an FFT calculated using a different API with different settings/variables in different apps.  similar- very similar- but different.
		
		//	normalize the vals in m_output, get the abs value
		float			tmpMax = 0.0;
		float			tmpMin = 99999999.0;
		for (const DataType & tmpFloat : m_output)	{
			DataType		tmpAbsVal = fabs(tmpFloat);
			tmpMax = std::max(tmpMax, tmpAbsVal);
			tmpMin = std::min(tmpMin, tmpAbsVal);
		}
		float			delta = tmpMax - tmpMin;
		
		//	this bit here runs through and copies the vals, normalizing them.
		//	this is commented out b/c this gives us a "2-up" result (it looks like two FFT graphs?)
		/*
		for (DataType & tmpFloat : m_output)	{
			tmpFloat = (fabs(tmpFloat) - tmpMin) / delta;
		}
		*/
		
		//	this bit runs through both of the "2-up" FFT graphs, averages the vals, and writes that to the first half of m_output, which is what we're going to upload
		
		for (int i=0; i<fftQuality; ++i)	{
			float		tmpVal = (fabs(m_output[i]) - tmpMin) / delta * 4.0;
			float		altVal = (fabs(m_output[i+fftQuality]) - tmpMin) / delta * 4.0;
			//m_output[i] = (tmpVal + altVal)/2.0;
			m_output[i] = std::max(tmpVal, altVal);
		}
		
	}
	
	//	make a CPU-based buffer, copy the single ISFAudioBufferList's values to it, start uploading it, update the local audio buffer
	GLBufferRef		tmpAudioBuffer = CreateBGRAFloatCPUBuffer(VVGL::Size(fftQuality*2,1));
	float			*rPtr = ablToProcess.floatPtr();
	float			*wPtr = reinterpret_cast<float*>( tmpAudioBuffer->cpuBackingPtr );
	GLBufferRef		newAudioBuffer = nullptr;
	
	if (rPtr!=nullptr && wPtr!=nullptr)	{
		//float			tmpMax = 0.0;
		//float			tmpMin = 99999999.0;
		for (int i=0; i<(fftQuality*2); ++i)	{
			
			float			tmpVal = ((*rPtr * 4.0f) + 4.0f) / 8.0f;
			//tmpMax = std::max(tmpMax, tmpVal);
			//tmpMin = std::min(tmpMin, tmpVal);
			*(wPtr+0) = tmpVal;
			*(wPtr+1) = tmpVal;
			*(wPtr+2) = tmpVal;
			*(wPtr+3) = 1.0;
			
			++rPtr;
			wPtr += 4;
		}
		//qDebug() << "min is " << tmpMin << ", max is " << tmpMax;
		newAudioBuffer = _audioUploader->streamCPUToTex(tmpAudioBuffer);
	}
	else
		qDebug() << "ERR: rPtr or wPtr null, " << __PRETTY_FUNCTION__;
	
	//	make a CPU-based buffer, copy the FFT result's values to it, start uploading it, update the local fft audio buffer
	GLBufferRef		tmpFFTBuffer = CreateBGRAFloatCPUBuffer(VVGL::Size(fftQuality,1));
	rPtr = m_output.data();
	wPtr = reinterpret_cast<float*>( tmpFFTBuffer->cpuBackingPtr );
	GLBufferRef		newFFTBuffer = nullptr;
	
	if (rPtr!=nullptr && wPtr!=nullptr)	{
		for (int i=0; i<(fftQuality); ++i)	{
			*(wPtr+0) = *rPtr;
			*(wPtr+1) = *rPtr;
			*(wPtr+2) = *rPtr;
			*(wPtr+3) = 1.0;
			
			++rPtr;
			wPtr += 4;
		}
		newFFTBuffer = _fftUploader->streamCPUToTex(tmpFFTBuffer);
	}
	
	//	update the local vars i have with the new images
	{
		std::lock_guard<recursive_mutex>		tmpLock(_lock);
		if (newAudioBuffer != nullptr)	{
			_audioBuffer = newAudioBuffer;
		}
		if (newFFTBuffer != nullptr)	{
			_fftBuffer = newFFTBuffer;
		}
		//	update the audio buffer, we may need it to calculate images with specific widths
		_lastABL = ablToProcess;
	}
}
GLBufferRef AudioController::getAudioImageBuffer(const int & inWidth)	{
	GLBufferRef		cpuBuffer = nullptr;
	{
		std::lock_guard<recursive_mutex>		tmpLock(_lock);
		//	if the width is 0 or less, just return _audioBuffer, which is being automatically calculated anyway
		if (inWidth < 1)
			return _audioBuffer;
		//	if the last ABL doesn't have any frames or channels, bail- something's wrong and we can't do anything
		if (_lastABL.numberOfFrames<(fftQuality*2) || _lastABL.numberOfChannels<1)
			return _audioBuffer;
	
		//	...if we're here, the caller wants an audio image buffer with a particular width, which we have to create on the fly.  we also have an audio buffer that we can use to calculate this value.
	
		int				rawResultsCount = fftQuality * 2;
		VVGL::Size		newBufferSize(std::max(1, std::min(inWidth, rawResultsCount)), _lastABL.numberOfChannels);
		cpuBuffer = CreateBGRAFloatCPUBuffer(newBufferSize);
		if (cpuBuffer == nullptr)
			return _audioBuffer;
		//	figure out how many vals from the buffer we need to combine to create every result val
		int				valsPerAvg = int( round( float(rawResultsCount)/float(newBufferSize.width) ) );
		float			*ablBufferData = _lastABL.floatPtr();
		//	run through every row of the output image, populating the pixels one at a time
		for (int rowIndex=0; rowIndex<newBufferSize.height; ++rowIndex)	{
			//	get a new wPtr for each row
			float			*wPtr = reinterpret_cast<float*>( cpuBuffer->cpuBackingPtr );
			wPtr += (rowIndex * int(sizeof(float)) * int(newBufferSize.width) );
		
			//	run through each column in the output image, populating the pixels one at a time
			for (int colIndex=0; colIndex<newBufferSize.width; ++colIndex)	{
				float			*rPtr = ablBufferData + (colIndex * valsPerAvg);
				//	make sure that we don't try to read outside the bounds of the buffer
				if (((colIndex+1)*valsPerAvg) >= rawResultsCount)
					rPtr = ablBufferData + (rawResultsCount - valsPerAvg);
				//	calculate the average value
				double			avgVal = 0.0;
				for (int i=0; i<valsPerAvg; ++i)	{
					avgVal += double( ((*rPtr * 4.0f) + 4.0f) / 8.0f );
					++rPtr;
				}
				avgVal /= double(valsPerAvg);
			
				//	write the avg val to the pixel
				*(wPtr+0) = float(avgVal);
				*(wPtr+1) = float(avgVal);
				*(wPtr+2) = float(avgVal);
				*(wPtr+3) = float(avgVal);
			
				//	increment the write ptr
				wPtr += 4;
			}
		
		}
	}
	//	upload the cpu buffer to a gl texture immediately (don't stream), return the GL texture
	return _audioUploader->uploadCPUToTex(cpuBuffer);
	
}
GLBufferRef AudioController::getAudioFFTBuffer(const int & inWidth)	{
	GLBufferRef		cpuBuffer = nullptr;
	{
		std::lock_guard<recursive_mutex>		tmpLock(_lock);
		//	if the width is 0 or less, just return _audioBuffer, which is being automatically calculated anyway
		if (inWidth < 1)
			return _fftBuffer;
	}
	return _fftUploader->uploadCPUToTex(cpuBuffer);
}










AudioController * GetAudioController()	{
	if (globalAudioController == nullptr)	{
		globalAudioController = new AudioController();
	}
	return globalAudioController;
}
