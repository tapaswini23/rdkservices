#include "AudioPlayer.h"
#include "logger.h"
#include <gst/app/gstappsrc.h>

#include <cmath>
#define AUDIO_GST_FRAGMENT_MAX_SIZE     (128 * 1024)
#define PLAYBACK_STARTED "PLAYBACK_STARTED"
#define PLAYBACK_FINISHED "PLAYBACK_FINISHED"
#define PLAYBACK_PAUSED "PLAYBACK_PAUSED"
#define PLAYBACK_RESUMED "PLAYBACK_RESUMED"
#define NETWORK_ERROR "NETWORK_ERROR"
#define PLAYBACK_ERROR "PLAYBACK_ERROR"
#define NEED_DATA "NEED_DATA"

GMainLoop* AudioPlayer::m_main_loop=NULL;
GThread* AudioPlayer::m_main_loop_thread=NULL;
SAPEventCallback* AudioPlayer::m_callback=NULL;
audio_hw_device_t* AudioPlayer::m_audio_dev=NULL;
//TODO Dock primary volume , if both APP & SYSTEM mode are playing
//static bool app_playing =false;
//static bool sys_playing =false;

AudioPlayer::AudioPlayer(AudioType audioType,SourceType sourceType,PlayMode playMode,int objectIdentifier)
{
    this->audioType = audioType;
    this->sourceType = sourceType;
    this->playMode = playMode;
    this->objectIdentifier = objectIdentifier;
    m_isPaused = false;
    state = READY;
    SAPLOG_INFO("SAP: AudioPlayer Constructor\n");    
    if(sourceType == DATA || sourceType == WEBSOCKET)
    {
        m_running = true;
        appsrc_firstpacket = true;
        webClient = NULL;
        bufferQueue = new BufferQueue(1000);
        m_thread= new std::thread(&AudioPlayer::PushDataAppSrc, this);
    }

    if(this->audioType == PCM)
    {
        m_PCMFormat = "S16LE";
        m_Layout = "interleaved";
        if(playMode == SYSTEM)
        {
            m_Rate = 44100;
            m_Channels = 2;
        }
        else
        {
            // tts-mode for local tts
            m_Rate = 22050;
            m_Channels = 1;
        }

    }

    createPipeline();
    SAPLOG_INFO("AudioPlayer AudioType:%d,SourceType:%d,playMode:%d,object id:%d\n",getAudioType(),getSourceType(),getPlayMode(),getObjectIdentifier());
    //Set mixter levels, this can be reflected when playing
    m_primVolume = DEFAULT_PRIM_VOL_LEVEL;
    m_thisVolume = DEFAULT_PLAYER_VOL_LEVEL;
    m_prevPrimVolume = m_prevThisVolume = -1;
}

AudioPlayer::~AudioPlayer()
{
    SAPLOG_INFO("SAP: AudioPlayer Destructor\n");
    if(sourceType == DATA || sourceType == WEBSOCKET)
    {   
        m_running = false;       
        bufferQueue->preDelete();
	SAPLOG_INFO("SAP: AudioPlayer Destructor before Pushapp src thread join player id %d\n",getObjectIdentifier());
	m_thread->join();
	SAPLOG_INFO("SAP: AudioPlayer Destructor after Pushapp src thread join player id %d\n",getObjectIdentifier());
        delete bufferQueue;
        delete m_thread;
    }  
    gst_element_set_state (m_pipeline, GST_STATE_NULL);
    gst_object_unref (m_pipeline);  
}

void AudioPlayer::Init(SAPEventCallback *callback)
{
    SAPLOG_INFO("SAP: AudioPlayer Init\n");
    if(!gst_is_initialized())
        gst_init(NULL,NULL);
    m_main_loop_thread = g_thread_new("BusWatch", (void* (*)(void*)) event_loop, NULL);
    m_callback = callback;
#if defined(PLATFORM_AMLOGIC)
    if(!m_audio_dev)
    {
        int ret = audio_hw_load_interface(&m_audio_dev);
        if (ret) 
        {
            SAPLOG_ERROR("SAP: Amlogic audio_hw_load_interface failed:%d, can not control mix gain", ret);
            return;
        }
        int inited = m_audio_dev->init_check(m_audio_dev);
        if (inited) 
        {
            SAPLOG_ERROR("SAP: Amlogic audio device not inited, can not control mix gain\n");
            audio_hw_unload_interface(m_audio_dev);
            m_audio_dev = NULL;
            return;
        }  
        SAPLOG_INFO("SAP: Amlogic audio device loaded, can control mix gain");
        return ;
    }
#endif
}

void AudioPlayer::DeInit()
{   
    SAPLOG_INFO("SAP: AudioPlayer DeInit\n");
    if(g_main_loop_is_running(m_main_loop))
        g_main_loop_quit(m_main_loop);
    g_thread_join(m_main_loop_thread);
   
#if defined(PLATFORM_AMLOGIC)
    if(m_audio_dev)
    {
        audio_hw_unload_interface(m_audio_dev);
        m_audio_dev = NULL;
    }
#endif
    SAPLOG_INFO("SAP: AudioPlayer DeInit last\n");
}

void AudioPlayer::event_loop()
{
    m_main_loop = g_main_loop_new(NULL, false);
    g_main_loop_run(m_main_loop);
}

//Set PCM audio Caps
bool AudioPlayer::configPCMCaps(const std::string format, int rate, int channels, const std::string layout)
{
    SAPLOG_INFO("configPCMCaps\n");
    //Extra check
    if( isPlaying())
    {
        SAPLOG_INFO ( "Can not set audio caps when playing");
        return false;
    }

    GstCaps *audiocaps = NULL;
    if(audioType == PCM)
    {
        audiocaps = getPCMAudioCaps( format,rate,channels,layout);

        if(audiocaps == NULL)
        {
            SAPLOG_INFO("SAP: Unable to add audio caps[ format=%s rate=%d channels=%d layout=%s for PCM audio.\n",format.c_str(),rate,channels,layout.c_str());
            return false;
        }

        if(sourceType == DATA || sourceType == WEBSOCKET)
        {
            gst_app_src_set_caps(GST_APP_SRC(m_source), audiocaps);
            gst_caps_unref(audiocaps);
        }
        else
        {
            if (m_capsfilter)
            {
                g_object_set (G_OBJECT (m_capsfilter), "caps", audiocaps, NULL);
                gst_caps_unref(audiocaps);
            }
            else
            {
                SAPLOG_ERROR( "SAP: Unable to set capsfilter for PCM audio, using default\n");
                return false;
            }
        }

    //If we set cofigs save it
    m_PCMFormat = format;
    m_Layout = layout;
    m_Rate = rate;
    m_Channels = channels;
    SAPLOG_INFO("SAP: PCM config is applied successfully format=%s layout=%s rate=%d channels=%d\n",m_PCMFormat.c_str() , m_Layout.c_str() , m_Rate , m_Channels);
    return true;
    }
    else 
    {
        SAPLOG_ERROR("SAP:  Can not set for  audio filter for this type of  audio =%d",audioType );
        return false;
    }
}

//Get a new audio caps , who uses has to release this caps
GstCaps * AudioPlayer::getPCMAudioCaps( const std::string format, int rate, int channels, const std::string layout)
{
    SAPLOG_INFO("SAP:  PCM config format=%s rate=%d channels=%d, layout=%s",format.c_str(),rate,channels,layout.c_str());
    GstCaps *audiocaps = NULL;
    audiocaps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, format.c_str(), "rate", G_TYPE_INT, rate,
                                "channels", G_TYPE_INT, channels, "layout", G_TYPE_STRING, layout.c_str(), NULL);
    if(audiocaps == NULL)
        {
            SAPLOG_INFO("SAP: Unable to add audio caps for PCM audio.\n");
        }
    return audiocaps;
}

bool AudioPlayer::isPlaying()
{
    //TODO Handle error as not Playing
    bool playing = false;
    if( m_pipeline)
    {
        if((sourceType == HTTPSRC || sourceType == FILESRC ) && (state == PLAYING )) playing = true;
        else if(( sourceType ==  WEBSOCKET || sourceType == DATA ) && (state == PLAYING  && !appsrc_firstpacket )) playing = true;
    }
    return playing;
}

void AudioPlayer::createPipeline()
{
    GstCaps *audiocaps = NULL;
    SAPLOG_INFO("SAP: Creating Pipeline...\n");
    m_pipeline = gst_pipeline_new(NULL);
    if (!m_pipeline) {

        SAPLOG_ERROR("SAP: Failed to create gstreamer pipeline player id:%d\n",getObjectIdentifier());
        return;
    }
     // create soc specific elements..generic elements
#if defined(PLATFORM_AMLOGIC)
    GstElement *convert = gst_element_factory_make("audioconvert", NULL);
    GstElement *resample = gst_element_factory_make("audioresample", NULL);
    m_audioSink = gst_element_factory_make("amlhalasink", NULL);
    m_audioVolume = m_audioSink;
#else
    m_audioSink = gst_element_factory_make("autoaudiosink", NULL); 
#endif 
    if(sourceType == HTTPSRC)
    {
       m_source = gst_element_factory_make("souphttpsrc", NULL);
    }
    else if(sourceType == FILESRC)
    {
       m_source = gst_element_factory_make("filesrc", NULL);
    }
    else if(sourceType == DATA || sourceType == WEBSOCKET)
    {
       //appsrc
       m_source = gst_element_factory_make ("appsrc", NULL);
       gst_app_src_set_max_bytes((GstAppSrc *)m_source,512000);
    }

    bool result = TRUE; 
    if(audioType == PCM)
    {   
        audiocaps = getPCMAudioCaps( m_PCMFormat, m_Rate , m_Channels ,m_Layout );
        if(audiocaps == NULL)
        {
            SAPLOG_INFO("Unable to add audio caps for PCM audio.\n");
            return;
        }
    
        if(playMode == SYSTEM)
	{
	    #if defined(PLATFORM_AMLOGIC)
            g_object_set(G_OBJECT(m_audioSink), "direct-mode", FALSE, NULL);
            #endif
	}
	else
	{
	    //PlayMode ->Apps....for TTS playback
	    #if defined(PLATFORM_AMLOGIC)
            g_object_set(G_OBJECT(m_audioSink), "tts-mode", TRUE, NULL);
            #endif
	}

	if(sourceType == DATA || sourceType == WEBSOCKET)
        {
            gst_app_src_set_caps(GST_APP_SRC(m_source), audiocaps);
	    gst_caps_unref(audiocaps);
	    //g_signal_connect (m_source, "need-data", G_CALLBACK (start_feed), this);
            //g_signal_connect (m_source, "enough-data", G_CALLBACK (stop_feed), this);
	    g_object_set(m_source, "format", GST_FORMAT_TIME, NULL);
	    #if defined(PLATFORM_AMLOGIC)
            gst_bin_add_many(GST_BIN(m_pipeline), m_source, convert, resample, m_audioSink, NULL);
            result = gst_element_link_many (m_source,convert,resample,m_audioSink,NULL);
	    #else
            gst_bin_add_many(GST_BIN(m_pipeline), m_source, m_audioSink, NULL);
            result = gst_element_link_many (m_source,m_audioSink,NULL);
            #endif
        }
        else
	{
            m_capsfilter = gst_element_factory_make ("capsfilter", NULL);
            if (m_capsfilter)
            {
                g_object_set (G_OBJECT (m_capsfilter), "caps", audiocaps, NULL);
                gst_caps_unref(audiocaps);
            }
            else
            {
                SAPLOG_ERROR( "SAP: Unable to create capsfilter for PCM audio Player id:%d\n",getObjectIdentifier());
                return;
            }
	
            #if defined(PLATFORM_AMLOGIC)
	    gst_bin_add_many(GST_BIN(m_pipeline), m_source, m_capsfilter, convert, resample, m_audioSink, NULL);
            result = gst_element_link_many (m_source,m_capsfilter,convert,resample,m_audioSink,NULL);
            #else
	    gst_bin_add_many(GST_BIN(m_pipeline), m_source, m_capsfilter, m_audioSink, NULL);
	    result = gst_element_link_many (m_source,m_capsfilter,m_audioSink,NULL);
	    #endif	
        }
    }

    else if(audioType == WAV)
    {
        SAPLOG_INFO("SAP: Pipleine for wav audioType\n");
        if(playMode == SYSTEM)
        {
            #if defined(PLATFORM_AMLOGIC)
            g_object_set(G_OBJECT(m_audioSink), "direct-mode", FALSE, NULL);
            #endif
        }
        else
        {
            //PlayMode ->Apps....for TTS playback
            #if defined(PLATFORM_AMLOGIC)
            g_object_set(G_OBJECT(m_audioSink), "tts-mode", TRUE, NULL);
            #endif
        }
        GstElement *wavparser = gst_element_factory_make("wavparse", NULL);
        #if defined(PLATFORM_AMLOGIC)
        gst_bin_add_many(GST_BIN(m_pipeline), m_source, wavparser, convert, resample, m_audioSink, NULL);
        result = gst_element_link_many (m_source,wavparser,convert,resample,m_audioSink,NULL);
        #endif
    }

    else
    {  
        //mp3
        SAPLOG_INFO("SAP: Pipleine for mp3 audioType\n");
        #if defined(PLATFORM_AMLOGIC)
        GstElement *parser = gst_element_factory_make("mpegaudioparse", NULL);
        GstElement *decodebin = gst_element_factory_make("avdec_mp3", NULL);
	gst_bin_add_many(GST_BIN(m_pipeline), m_source, parser, decodebin, convert, resample, m_audioSink, NULL);
        result &= gst_element_link (m_source, parser);
        result &= gst_element_link (parser, decodebin);
        result &= gst_element_link (decodebin, convert);
        result &= gst_element_link (convert, resample);
        result &= gst_element_link (resample, m_audioSink);
        #endif
    }

    if(!result) 
    {
        SAPLOG_ERROR("SAP: Failed to link element Player id %d\n",getObjectIdentifier());
        gst_object_unref(m_pipeline);
        m_pipeline = NULL;
        return;
    }

    GstBus *bus = gst_element_get_bus(m_pipeline);
    m_busWatch = gst_bus_add_watch(bus, GstBusCallback, (gpointer)(this));
    gst_object_unref(bus);
    SAPLOG_INFO("SAP: End of create pipeline Player id: %d\n",getObjectIdentifier());

}

int AudioPlayer::GstBusCallback(GstBus *, GstMessage *message, gpointer data) 
{
    AudioPlayer *player  = (AudioPlayer*) data;
    return player->handleMessage(message);
}


gboolean AudioPlayer::PushDataAppSrc()
{
    while(m_running)
    {	    
        Buffer *buffer =NULL;
        if(bufferQueue->isEmpty())
        {
             if(!appsrc_firstpacket)
             {
	         //event -->Underflow
                 m_callback->onSAPEvent(getObjectIdentifier(),NEED_DATA);
             }
        }
        size_t maxBytes = AUDIO_GST_FRAGMENT_MAX_SIZE;
        //package should be played as soon as it arrived
        //GstClockTime pts = 0;
        //GstClockTime dts = 0;
        buffer = bufferQueue->remove();  //blocking call
	if(buffer == NULL)
	{
            continue;		
	}
        int length = buffer->getLength();
        char *ptr = buffer->getBuffer();

        while(length != 0)
        {               
            int lenToSend = length;
        
            if(lenToSend > maxBytes)
            {
                lenToSend = maxBytes;
            }
     
            GstBuffer *gbuffer = gst_buffer_new_and_alloc((guint)lenToSend);
            GstMapInfo map;
            gst_buffer_map(gbuffer, &map, GST_MAP_WRITE);
            memcpy(map.data,ptr,lenToSend);
            gst_buffer_unmap(gbuffer, &map);
            //GST_BUFFER_PTS(gbuffer) = pts;
            //GST_BUFFER_DTS(gbuffer) = dts;
            //GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(player->m_source), gbuffer);
            GstFlowReturn ret;
            g_signal_emit_by_name (m_source, "push-buffer", gbuffer, &ret);

            gst_buffer_unref (gbuffer);

            if (ret != GST_FLOW_OK)
            {
	        SAPLOG_WARNING("SAP: appsrc not accepting buffer\n");
            }
            if(appsrc_firstpacket)
            {
                appsrc_firstpacket = false;
                m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_STARTED);
                setPrimaryVolume(m_primVolume);
                setVolume(m_thisVolume);
            }
            ptr = lenToSend + (char *)ptr;
            length -= lenToSend;
        }
        buffer->deleteBuffer();
	delete buffer;        
    }
   
}
static gboolean pop_data(AudioPlayer *player)
{
    return player->PushDataAppSrc();
}

void AudioPlayer::wsConnectionStatus(WSStatus status)
{
    switch (status)
    {
        case CONNECTED:     break;
        case DISCONNECTED:  break;
        case NETWORKERROR: m_callback->onSAPEvent(getObjectIdentifier(),NETWORK_ERROR); break;
    } 
}

void AudioPlayer::push_data(const void *ptr,int length)
{
    if(!bufferQueue->isFull())
    {
	Buffer *buffer = new Buffer();
        buffer->fillBuffer(ptr,length);
        bufferQueue->add(buffer);
    }
}

bool AudioPlayer::handleMessage(GstMessage *message) 
{
    GError* error = NULL;
    gchar* debug = NULL;
    if(!m_pipeline) 
    {
        SAPLOG_ERROR("SAP: NULL Pipeline...\n"); 
        return false;
    }
    switch (GST_MESSAGE_TYPE(message)){

        case GST_MESSAGE_ERROR: {
                gst_message_parse_error(message, &error, &debug);
                SAPLOG_ERROR("error! code: %d, %s, Debug: %s", error->code, error->message, debug);
                GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "error-pipeline");
                std::string source = GST_MESSAGE_SRC_NAME(message);
                if(source.find("httpsrc") != std::string::npos)
                {
                    SAPLOG_INFO("Network Error event on id:%d\n",getObjectIdentifier());
                    m_callback->onSAPEvent(getObjectIdentifier(),NETWORK_ERROR);
                }
                else
                {
                    SAPLOG_INFO("Playback Error event on id:%d\n",getObjectIdentifier());
                    m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_ERROR);
                }
                state = PLAYBACKERROR;
                if(sourceType == WEBSOCKET || sourceType == DATA)
                {
                    Stop();
                }
		
            }
            break;

        case GST_MESSAGE_WARNING: {
                gst_message_parse_warning(message, &error, &debug);
                SAPLOG_WARNING("warning! code: %d, %s, Debug: %s", error->code, error->message, debug);
            }
            break;

        case GST_MESSAGE_EOS: {
                SAPLOG_INFO("Audio EOS message received");
                if(state != PLAYBACKERROR)
                {
                    appsrc_firstpacket = true;
                    SAPLOG_INFO("Playback Finished event\n");
                    m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_FINISHED);
                    state = READY;
                }
            }
            break;

        case GST_MESSAGE_DURATION_CHANGED: {
                gst_element_query_duration(m_pipeline, GST_FORMAT_TIME, &m_duration);
                SAPLOG_INFO("Duration %" GST_TIME_FORMAT, GST_TIME_ARGS(m_duration));
            }
            break;

        case GST_MESSAGE_STATE_CHANGED: {
                gchar* filename;
                GstState oldstate, newstate, pending;
                gst_message_parse_state_changed (message, &oldstate, &newstate, &pending);

                //Ignore messages not coming directly from the pipeline.
                if (GST_ELEMENT(GST_MESSAGE_SRC(message)) != m_pipeline)
                    break;

                filename = g_strdup_printf("%s-%s", gst_element_state_get_name(oldstate), gst_element_state_get_name(newstate));
                GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, filename);
                g_free(filename);

                // get the name and state
                SAPLOG_INFO("%s old_state %s, new_state %s, pending %s",
                        GST_MESSAGE_SRC_NAME(message) ? GST_MESSAGE_SRC_NAME(message) : "",
                        gst_element_state_get_name (oldstate), gst_element_state_get_name (newstate), gst_element_state_get_name (pending));

                if (oldstate == GST_STATE_NULL && newstate == GST_STATE_READY) {

                } else if (oldstate == GST_STATE_READY && newstate == GST_STATE_PAUSED) {

                    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "paused-pipeline");
		    state = PAUSED;
                } else if (oldstate == GST_STATE_PAUSED && newstate == GST_STATE_PAUSED) {
			state = PAUSED;
                } else if (oldstate == GST_STATE_PAUSED && newstate == GST_STATE_PLAYING) {
                    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "playing-pipeline");
			   
			    SAPLOG_INFO("moved to playing state id:%d\n",getObjectIdentifier());
			    state = PLAYING;
                            if(sourceType != WEBSOCKET && sourceType != DATA)
                            {
                                if(m_isPaused)
                                {
                                    m_isPaused = false;
                                    SAPLOG_INFO("Playback Resume event on id:%d\n",getObjectIdentifier());
                                    //playback resume event
                                    m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_RESUMED);
                                }  
                                else
                                {
                                    //playback start event
                                    SAPLOG_INFO("Playback started event on id:%d\n",getObjectIdentifier());
                                    m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_STARTED);
                                }
                                //Multiple player might have set different primary volume.
                                //But we set only the primary volume set by the newest player
                                setPrimaryVolume(m_primVolume);
                                setVolume(m_thisVolume);
                                //Lower primary volume
                            }

                } else if (oldstate == GST_STATE_PLAYING && newstate == GST_STATE_PAUSED) {
			state = PAUSED;
                        if(m_isPaused)
                        {
                             //playback paused event
                             SAPLOG_INFO("Playback Paused event on id:%d\n",getObjectIdentifier());
                             m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_PAUSED);
                        }
                  
                } else if (oldstate == GST_STATE_PAUSED && newstate == GST_STATE_READY) {
                } else if (oldstate == GST_STATE_READY && newstate == GST_STATE_NULL) {
                           state = READY;
                }
            }
            break;

        default:
            break;
    }

    if(error)
        g_error_free(error);

    if(debug)
        g_free(debug);
     //READY state will be set when stoping player Close() & Stop()
    if( state == PAUSED || state == PLAYBACKERROR || state == READY   )
    {
      SAPLOG_INFO(" Playback current state=%d ",state.load());
      //if neither APP or SYSTEM mode is playing  now, then set primary volume to Max
      //TODO if( !app_playing && !sys_playing )
        setPrimaryVolume ( MAX_PRIM_VOL_LEVEL );
    }

    return true;
}

void AudioPlayer::resetPipeline()
{
    SAPLOG_WARNING("Resetting Pipeline...player id %d\n",getObjectIdentifier());

    // Detect pipe line error and destroy the pipeline if any
    if(state == PLAYBACKERROR) 
    {
        SAPLOG_WARNING("Pipeline error occured, attempting to recover by re-creating pipeline");
        // Try to recover from errors by destroying the pipeline
        destroyPipeline();
    }
    m_isPaused = false;
    if(!m_pipeline) 
    {
        // If pipe line is NULL, create one
        createPipeline();
    } 
    else 
    {
        // If pipeline is present, bring it to NULL state
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        while(!waitForStatus(GST_STATE_NULL, 300));
    }
}

bool AudioPlayer::waitForStatus(GstState expected_state, uint32_t timeout_ms) 
{
    if(m_pipeline) 
    {
        GstState state;
        GstState pending;

        std::unique_lock<std::mutex> mlock(m_queueMutex);
        auto timeout = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms);
        m_condition.wait_until(mlock, timeout, [this, &state, &pending, expected_state] () {                 
                    gst_element_get_state(m_pipeline, &state, &pending, GST_CLOCK_TIME_NONE);
                    if(state == expected_state)
                    {
                        SAPLOG_INFO("SAP: Excepted state matched without timeout\n");
                        return true;
                    }
                    SAPLOG_INFO("SAP: state timeout\n");
                    return false;
                });
        return true;
    }
    return true;
}

void AudioPlayer::destroyPipeline()
{
    SAPLOG_WARNING("SAP: Destroying Pipeline...Player id %d\n",getObjectIdentifier());

    if(m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        waitForStatus(GST_STATE_NULL, 200);
        gst_object_unref(m_pipeline);
    }
    m_pipeline = NULL;
}

void AudioPlayer::Play(std::string url)
{
    std::lock_guard<std::mutex> lock(m_playMutex);
    m_url = url;   
    SAPLOG_INFO("SAP: AudioPlayer Play invoked Playerid %d..URL %s\n",getObjectIdentifier(),m_url.c_str());
    if(m_pipeline)
    {
        Stop();
        if(sourceType == HTTPSRC || sourceType == FILESRC)
        {
            g_object_set(G_OBJECT(m_source), "location", m_url.c_str(), NULL);
        }
        if(sourceType == WEBSOCKET)
        { 
            webClient = new WebSocketClient(this);
            webClient->connect(m_url);
        } 
        SAPLOG_INFO("SAP: PLAYING GLOBAL primary Volume=%d player Volume=%d",m_primVolume  , m_thisVolume ); 
        //TODO setAppSysPlayingSate(true)
        gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    }    
}
   
void AudioPlayer::PlayBuffer(const char *data,int length)
{  
    std::lock_guard<std::mutex> lock(m_apiMutex);
    SAPLOG_INFO("SAP: AudioPlayer PlayBuffer invoked Playerid %d\n",getObjectIdentifier());
    if(m_pipeline)
    {
        if(state != PLAYING)
            gst_element_set_state(m_pipeline, GST_STATE_PLAYING);      
        push_data(data,length);
    }
}

bool AudioPlayer::Pause()
{
    std::lock_guard<std::mutex> lock(m_apiMutex);
    SAPLOG_INFO("SAP: AudioPlayer Pause Playerid %d\n",getObjectIdentifier());
    if( m_pipeline )
    {

        if(sourceType == FILESRC || sourceType == HTTPSRC)
        {
	   
            if(!m_isPaused && state == PLAYING) 
            {
                m_isPaused = true;
                SAPLOG_INFO("SAP: AudioPlayer Pause invoked\n");
                gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
                return true;
            } 
        }
    }
    return false;
}

void AudioPlayer::Stop()
{
    SAPLOG_INFO("SAP: AudioPlayer Stop Playerid %d\n",getObjectIdentifier());
    std::lock_guard<std::mutex> lock(m_apiMutex);
    if(sourceType == DATA || sourceType == WEBSOCKET )
    {
        if(webClient != NULL)
        {
            webClient->disconnect();
            delete webClient;
            webClient = NULL;
        }

        appsrc_firstpacket = true;
        bufferQueue->clear();
	SAPLOG_INFO("size of Buffer queue after clear %d\n",bufferQueue->count());
	  
    }
    resetPipeline();
    state = READY;
    
}
bool AudioPlayer::Resume()
{  
    std::lock_guard<std::mutex> lock(m_apiMutex);
    SAPLOG_INFO("SAP: AudioPlayer Resume Playerid %d\n",getObjectIdentifier());
    if(m_pipeline )
    {
        if(sourceType == FILESRC || sourceType == HTTPSRC)
        {
            if(m_isPaused && state == PAUSED)
            {
                SAPLOG_INFO("SAP: AudioPlayer Resume invoked\n");
                gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
                return true;
            } 
        }
    }
    return false;    
}
AudioType AudioPlayer::getAudioType()
{
    return audioType;
}

PlayMode AudioPlayer::getPlayMode()
{
    return playMode;
}

SourceType AudioPlayer::getSourceType()
{
    return sourceType;
}

int AudioPlayer::getObjectIdentifier()
{
    return objectIdentifier;
}

bool AudioPlayer::loadInitAudioDev()
{
    //TTSLOG_WARNING("Destroying Pipeline...");

    int ret = audio_hw_load_interface(&m_audio_dev);
    if (ret) {
        //TRACE(Trace::Error, (_T("Failed to delete %s\n"), fullPath.c_str()));
        //TRACE(Trace::Error, (_T("Amlogic audio_hw_load_interface failed:%d, can not control mix gain\n"), ret));
        return false;
    }
    int inited = m_audio_dev->init_check(m_audio_dev);
    if (inited) {
       // TRACE(Trace::Error, (_T("Amlogic audio device not inited, can not control mix gain\n")));
        audio_hw_unload_interface(m_audio_dev);
        m_audio_dev = NULL;
        return false;
    }
  //TRACE(Trace::Information, (_T("Amlogic audio device loaded, can control mix gain\n")));
  return true;
}

/*
 * Control gain of
 * primary audio (direct-mode=true),
 * system audio  (direct-mode=false)
 * app audio     (tts-mode=true)
 * mixgain value from 0 to -96  in db
 * 0   -> Maximum
 * -96 -> Minimum
*/
bool AudioPlayer::setMixGain(MixGain gain, int val)
{
     int ret;
     bool status = false;
     char mixgain_cmd[32];
     if(gain == MIXGAIN_PRIM)
         snprintf(mixgain_cmd, sizeof(mixgain_cmd), "prim_mixgain=%d", val);
        else if( gain == MIXGAIN_SYS )
                snprintf(mixgain_cmd, sizeof(mixgain_cmd), "syss_mixgain=%d",val);
        else if(gain == MIXGAIN_TTS)
                snprintf(mixgain_cmd, sizeof(mixgain_cmd), "apps_mixgain=%d",val);
         else {
                SAPLOG_ERROR("SAP: Unsuported Gain type=%d",gain);
                return false;
        }

      if(m_audio_dev) {
         ret = m_audio_dev->set_parameters(m_audio_dev, mixgain_cmd );
         if(!ret) {
             SAPLOG_INFO("SAP: Amlogic audio dev  set param=%s success\n",mixgain_cmd);
             status = true;
         }
          else {
                SAPLOG_ERROR("SAP: Amlogic audio dev  set_param=%s failed  error=%d\n",mixgain_cmd,ret);
                status = false;
          }
     }
 return status;
}

//Primary Audio control/dock
void AudioPlayer::setPrimaryVolume( int primVol)
{
#ifdef PLATFORM_AMLOGIC
    if( m_prevPrimVolume != primVol )
    {
        //Prim Mix gain
        double primVolGain = (double)primVol/100;
        //convert voltage gain/loss to db
        double primdbOut = round(1000000*20*(std::log(primVolGain)/std::log(10)))/1000000;
        setMixGain(MIXGAIN_PRIM,round(primdbOut));
        m_prevPrimVolume = primVol;
    }
#else
    //TODO for Other platforms
    SAPLOG_ERROR(" Audio docking not Implemented" );
#endif
    return;
}

//Player audio control
void AudioPlayer::setVolume( int thisVol)
{
    SAPLOG_INFO(" Prev Player Volume=%d cur Vol=%d",m_prevThisVolume , thisVol );
#ifdef PLATFORM_AMLOGIC
    if(audioType == PCM || audioType == WAV)
    {
        if( m_prevThisVolume != thisVol)
        { 
            double thisvolGain = (double)thisVol/100;
            //convert voltage gain/loss to db
            double dbOut = round(1000000*20*(std::log(thisvolGain)/std::log(10)))/1000000;
            if( playMode == SYSTEM)
                setMixGain(MIXGAIN_SYS,round(dbOut));
            else if( playMode == APP)
                setMixGain(MIXGAIN_TTS,round(dbOut));
            SAPLOG_INFO("SAP: Cur vol=%0.5f thisvolGain=%0.5f dbOut =%0.5f", thisVol,thisvolGain,dbOut);
            m_prevThisVolume = thisVol;
        } 
    }
    else if(audioType == MP3 )
    { 
        g_object_set(G_OBJECT(m_audioVolume), "stream-volume", (double)thisVol/100, NULL);
    }
#else
    g_object_set(G_OBJECT(m_audioVolume), "volume", (double)thisVol/100, NULL);
#endif
    return;
}

// Provision for Audio docking, only when app is speaking
void AudioPlayer::SetMixerLevels(int primVol, int thisVol)
{
    SAPLOG_INFO("SAP: primary Volume=%d player Volume=%d",primVol , thisVol );
    if(m_primVolume >=0 ) m_primVolume = primVol;
    if(m_thisVolume >=0)     m_thisVolume = thisVol;
    SAPLOG_INFO("SAP: GLOBAL primary Volume=%d player Volume=%d",m_primVolume  , m_thisVolume );
    //instant volume level change, if we are playing,else Play() time, we will do
    //This is to avoid, if play() is called later, do not set any volume level. It will give bad user experience.
    if(isPlaying())
    {
        SAPLOG_INFO("SAP: %s:%d PLAYING AUDIO NOW",__FUNCTION__ ,__LINE__ );
        setPrimaryVolume(m_primVolume );
        setVolume(m_thisVolume);
    }
    return;
}

#if 0
void AudioPlayer::setAppSysPlayingSate(bool state)
{
   if( sourceType  == APP ) app_playing = state ;
   if( sourceType  == SYSTEM  ) sys_playing = state;

}
#endif

