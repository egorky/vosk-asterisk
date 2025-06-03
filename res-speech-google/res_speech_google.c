/*
 * Asterisk -- An open source telephony toolkit.
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 *
 * Please follow coding guidelines
 * http://svn.digium.com/view/asterisk/trunk/doc/CODING-GUIDELINES
 */

/*! \file
 *
 * \brief Implementation of the Asterisk's Speech API via Google Speech gRPC
 *
 * \author Your Name Here <you@example.com>
 *
 * \ingroup applications
 */

/* Asterisk includes. */
#include "asterisk.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h" // For ast_channel, ast_format
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/frame.h"   // For ast_frame
#include "asterisk/speech.h"
#include "asterisk/format_cache.h" // For ast_format_cap_append, ast_format_slin
#include "asterisk/json.h"         // Might be used for parsing results later
#include "asterisk/lock.h"         // For ast_mutex_lock, etc. if needed
#include "asterisk/utils.h"        // For ast_strdup, ast_calloc, ast_free
#include "asterisk/cli.h"          // For CLI commands, if any are added later

// Google Cloud Speech-to-Text gRPC headers
#include "google/cloud/speech/v1/speech.grpc.pb.h"
#include "google/cloud/speech/v1/speech.pb.h"
#include <grpcpp/grpcpp.h>
#include "google/cloud/grpc_error_delegate.h"
#include "google/cloud/status_or.h"
#include "google/cloud/common_options.h"
#include "google/cloud/credentials.h"
#include "google/cloud/internal/common_options.h"
#include "google/cloud/options.h"


#define GOOGLE_ENGINE_NAME "google"
#define GOOGLE_ENGINE_CONFIG "res_speech_google.conf"
// #define GOOGLE_BUF_SIZE 3200 // May not be needed if streaming directly

/** \brief Forward declaration of speech (client object) */
typedef struct google_speech_t google_speech_t;
/** \brief Forward declaration of engine (global object) */
typedef struct google_engine_t google_engine_t;

/** \brief Declaration of Google Speech structure */
struct google_speech_t {
	char *name; // Name for logging
	void *speech_stub; // std::unique_ptr<google::cloud::speech::v1::Speech::StubInterface>
	void *context;     // std::shared_ptr<grpc::ClientContext>
	void *stream;      // std::unique_ptr<grpc::ClientReaderWriter<google::cloud::speech::v1::StreamingRecognizeRequest, google::cloud::speech::v1::StreamingRecognizeResponse>>
	char *last_result;
	char *service_account_key_path;
	char *language_code;
	int sample_rate_hertz;
	char *model;
	bool enable_automatic_punctuation;
	// Add other necessary state variables here, e.g., for interim results
	// int interim_results_enabled;
	void *channel_ptr; // Store std::shared_ptr<grpc::Channel>
};

/** \brief Declaration of Google recognition engine */
struct google_engine_t {
	char *default_language_code;
	char *default_service_account_key_path;
	char *default_model;
	bool default_enable_automatic_punctuation;
	int initialized; // Flag to check if config is loaded
};

static struct google_engine_t google_engine;

// Helper function to read a file into a string
static std::string read_key_file(const char* file_path, const char* log_name) {
    std::ifstream key_file(file_path);
    if (!key_file.is_open()) {
        ast_log(LOG_ERROR, "(%s) Failed to open service account key file: %s\n", log_name, file_path);
        return "";
    }
    std::stringstream buffer;
    buffer << key_file.rdbuf();
    return buffer.str();
}


static int google_engine_config_load(void); // Forward declaration

/** \brief Set up the speech structure within the engine */
static int google_recog_create(struct ast_speech *speech, struct ast_format *format)
{
	google_speech_t *google_speech;
	struct ast_format *native_format;

	if (!google_engine.initialized) {
		ast_log(LOG_WARNING, "Google Speech engine not initialized. Attempting to load config.\n");
		if (google_engine_config_load() != 0) {
			ast_log(LOG_ERROR, "Failed to load Google Speech engine configuration during create.\n");
			return -1;
		}
	}

	google_speech = (google_speech_t*)ast_calloc(1, sizeof(google_speech_t));
	if (!google_speech) {
		return -1;
	}
	google_speech->name = ast_strdup("google"); // Or derive from channel/speech object if desired
	speech->data = google_speech;

	// Copy defaults from global engine config
	if (google_engine.default_service_account_key_path) {
		google_speech->service_account_key_path = ast_strdup(google_engine.default_service_account_key_path);
	}
	if (google_engine.default_language_code) {
		google_speech->language_code = ast_strdup(google_engine.default_language_code);
	} else {
		// Fallback if not set in config (should have a default from config_load)
		google_speech->language_code = ast_strdup("en-US");
	}
	if (google_engine.default_model) {
		google_speech->model = ast_strdup(google_engine.default_model);
	} else {
		google_speech->model = ast_strdup("default");
	}
	google_speech->enable_automatic_punctuation = google_engine.default_enable_automatic_punctuation;

	// Determine sample rate from the provided format (from ast_channel_nativeformats)
	// The format passed to create() is usually the one Asterisk wants to provide data in.
	if (!format || !(google_speech->sample_rate_hertz = ast_format_get_sample_rate(format))) {
		ast_log(LOG_WARNING, "(%s) Invalid or missing format for speech recognition setup. Defaulting to 16000Hz.\n", google_speech->name);
		google_speech->sample_rate_hertz = 16000; // A common default
	}
	if (google_speech->sample_rate_hertz <= 0) { // Double check after ast_format_get_sample_rate
		ast_log(LOG_WARNING, "(%s) Invalid sample rate %d from provided format. Defaulting to 16000Hz.\n", google_speech->name, google_speech->sample_rate_hertz);
		google_speech->sample_rate_hertz = 16000; // A common default
	}


	ast_debug(1, "(%s) Create speech resource. Lang: %s, Rate: %dHz, Model: %s, Punct: %s\n",
		google_speech->name,
		google_speech->language_code,
		google_speech->sample_rate_hertz,
		google_speech->model,
		google_speech->enable_automatic_punctuation ? "true" : "false");

	// --- gRPC Channel and Stub Creation ---
	std::shared_ptr<grpc::ChannelCredentials> creds;
	std::shared_ptr<grpc::Channel> channel; // Will be stored in google_speech->channel_ptr

	try {
		if (google_speech->service_account_key_path && google_speech->service_account_key_path[0] != '\0') {
			ast_log(LOG_NOTICE, "(%s) Using service account key from: %s\n", google_speech->name, google_speech->service_account_key_path);
			std::string json_key_string = read_key_file(google_speech->service_account_key_path, google_speech->name);
			if (!json_key_string.empty()) {
				grpc::GoogleServiceAccountJWTAccessCredentialsOptions options;
				options.json_key = json_key_string;
				creds = grpc::GoogleServiceAccountJWTAccessCredentials(options);
			} else {
				ast_log(LOG_WARNING, "(%s) Failed to read service account key file, or file is empty. Falling back to Google Default Credentials.\n", google_speech->name);
				creds = grpc::GoogleDefaultCredentials();
			}
		} else {
			ast_log(LOG_NOTICE, "(%s) No service account key path set. Using Google Default Credentials.\n", google_speech->name);
			creds = grpc::GoogleDefaultCredentials();
		}
	} catch (const std::exception& e) {
		ast_log(LOG_ERROR, "(%s) Exception creating credentials: %s. Falling back to Google Default Credentials if possible.\n", google_speech->name, e.what());
		try {
			creds = grpc::GoogleDefaultCredentials();
		} catch (const std::exception& e_def) {
			ast_log(LOG_ERROR, "(%s) Exception creating Google Default Credentials: %s\n", google_speech->name, e_def.what());
			// creds will remain nullptr
		}
	}

	if (!creds) {
		ast_log(LOG_ERROR, "(%s) Failed to create any Google Cloud Credentials.\n", google_speech->name);
		ast_free(google_speech->name);
		ast_free(google_speech->service_account_key_path);
		ast_free(google_speech->language_code);
		ast_free(google_speech->model);
		ast_free(google_speech);
		speech->data = NULL;
		return -1;
	}

	channel = grpc::CreateChannel("speech.googleapis.com:443", creds);
	if (!channel) {
		ast_log(LOG_ERROR, "(%s) Failed to create gRPC channel to speech.googleapis.com.\n", google_speech->name);
		ast_free(google_speech->name);
		ast_free(google_speech->service_account_key_path);
		ast_free(google_speech->language_code);
		ast_free(google_speech->model);
		ast_free(google_speech);
		speech->data = NULL;
		return -1;
	}
	google_speech->channel_ptr = new auto(channel); // Store std::shared_ptr<grpc::Channel>

	try {
		auto stub = google::cloud::speech::v1::Speech::NewStub(channel);
		google_speech->speech_stub = new auto(std::move(stub)); // new std::unique_ptr<...>
		google_speech->context = new auto(std::make_shared<grpc::ClientContext>()); // new std::shared_ptr<...>
	} catch (const std::exception& e) {
		ast_log(LOG_ERROR, "(%s) Exception creating Speech Stub or ClientContext: %s\n", google_speech->name, e.what());
		delete static_cast<std::shared_ptr<grpc::Channel>*>(google_speech->channel_ptr); // Clean up channel
		if (google_speech->speech_stub) {
			delete static_cast<std::unique_ptr<google::cloud::speech::v1::Speech::StubInterface>*>(google_speech->speech_stub);
		}
		ast_free(google_speech->name);
		ast_free(google_speech->service_account_key_path);
		ast_free(google_speech->language_code);
		ast_free(google_speech->model);
		ast_free(google_speech);
		speech->data = NULL;
		return -1;
	}

	if (!google_speech->speech_stub || !google_speech->context) {
		ast_log(LOG_ERROR, "(%s) Failed to create Google Speech stub or client context.\n", google_speech->name);
		if (google_speech->speech_stub) delete static_cast<std::unique_ptr<google::cloud::speech::v1::Speech::StubInterface>*>(google_speech->speech_stub);
		if (google_speech->context) delete static_cast<std::shared_ptr<grpc::ClientContext>*>(google_speech->context);
		ast_free(google_speech->name);
		ast_free(google_speech->service_account_key_path);
		ast_free(google_speech->language_code);
		ast_free(google_speech);
		speech->data = NULL;
		return -1;
	}

	ast_debug(1, "(%s) Created Google Speech resource successfully.\n", google_speech->name);
	return 0;
}

/** \brief Destroy any data set on the speech structure by the engine */
static int google_recog_destroy(struct ast_speech *speech)
{
	google_speech_t *google_speech = (google_speech_t*)speech->data;
	if (!google_speech) {
		return 0;
	}

	ast_debug(1, "(%s) Destroy Google speech resource\n", google_speech->name);

	// Clean up gRPC stream if it exists
	if (google_speech->stream) {
		ast_log(LOG_DEBUG, "(%s) Cleaning up gRPC stream.\n", google_speech->name);
		try {
			auto stream_ptr = static_cast<std::unique_ptr<grpc::ClientReaderWriter<google::cloud::speech::v1::StreamingRecognizeRequest, google::cloud::speech::v1::StreamingRecognizeResponse>>*>(google_speech->stream);
			// Ensure WritesDone and Finish are called if stream was active.
			// (*stream_ptr)->WritesDone(); // Should be done in stop or finalize
			grpc::Status status = (*stream_ptr)->Finish(); // This blocks and returns status.
			if (!status.ok()) {
				ast_log(LOG_ERROR, "(%s) gRPC stream Finish failed: (%d) %s\n", google_speech->name, status.error_code(), status.error_message().c_str());
			} else {
				ast_debug(1, "(%s) gRPC stream Finished successfully in destroy.\n", google_speech->name);
			}
			delete stream_ptr;
			google_speech->stream = NULL;
		} catch (const std::exception& e) {
			ast_log(LOG_ERROR, "(%s) Exception during gRPC stream cleanup: %s\n", google_speech->name, e.what());
		}
	}

	// Clean up ClientContext
	if (google_speech->context) {
		delete static_cast<std::shared_ptr<grpc::ClientContext>*>(google_speech->context);
		google_speech->context = NULL;
	}

	// Clean up Speech Stub
	if (google_speech->speech_stub) {
		delete static_cast<std::unique_ptr<google::cloud::speech::v1::Speech::StubInterface>*>(google_speech->speech_stub);
		google_speech->speech_stub = NULL;
	}

	ast_free(google_speech->name);
	ast_free(google_speech->last_result);
	ast_free(google_speech->service_account_key_path);
	ast_free(google_speech->language_code);
	ast_free(google_speech);
	speech->data = NULL;

	return 0;
}

/*! \brief Stop the in-progress recognition */
static int google_recog_stop(struct ast_speech *speech)
{
	google_speech_t *google_speech = (google_speech_t*)speech->data;
	if (!google_speech) return 0;

	ast_debug(1, "(%s) Stop recognition called.\n", google_speech->name);

	if (google_speech->stream) {
		try {
			auto stream_ptr = static_cast<std::unique_ptr<grpc::ClientReaderWriter<google::cloud::speech::v1::StreamingRecognizeRequest, google::cloud::speech::v1::StreamingRecognizeResponse>>*>(google_speech->stream);
			// Signal that we are done sending audio data.
			if (!(*stream_ptr)->WritesDone()) {
				ast_log(LOG_WARNING, "(%s) gRPC stream WritesDone failed. This might mean the stream is already broken.\n", google_speech->name);
				// Proceed to change state anyway, Finish() in destroy will give final status.
			} else {
				ast_debug(1, "(%s) gRPC stream WritesDone called successfully.\n", google_speech->name);
			}
		} catch (const std::exception& e) {
			ast_log(LOG_ERROR, "(%s) Exception during WritesDone: %s\n", google_speech->name, e.what());
		}
		// Do not call Finish() here as it's blocking. Finish will be called in destroy.
		// Or, if Asterisk expects stop to be synchronous for final results, a read loop + Finish would be needed here.
		// For now, we assume an asynchronous model where destroy handles the final cleanup.
	}
	ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY); // Or AST_SPEECH_STATE_DONE if final results were processed here.
	return 0;
}

/*! \brief Load a local grammar on the speech structure */
static int google_recog_load_grammar(struct ast_speech *speech, const char *grammar_name, const char *grammar_path)
{
	google_speech_t *google_speech = (google_speech_t*)speech->data;
	if (!google_speech) return -1;
	ast_log(LOG_NOTICE, "(%s) Load grammar: %s from %s (not supported by Google Speech API, using PhraseSets if applicable)\n", google_speech->name, grammar_name, grammar_path);
	// Google Speech API uses SpeechContext / PhraseSets, not direct grammar loading like MRCP.
	// This might be mapped to updating the StreamingRecognizeConfig.
	return 0; // Or -1 if strict adherence to "grammar" is required.
}

/** \brief Unload a local grammar */
static int google_recog_unload_grammar(struct ast_speech *speech, const char *grammar_name)
{
	google_speech_t *google_speech = (google_speech_t*)speech->data;
	if (!google_speech) return -1;
	ast_log(LOG_NOTICE, "(%s) Unload grammar: %s (not directly applicable)\n", google_speech->name, grammar_name);
	return 0;
}

/** \brief Activate a loaded grammar */
static int google_recog_activate_grammar(struct ast_speech *speech, const char *grammar_name)
{
	google_speech_t *google_speech = (google_speech_t*)speech->data;
	if (!google_speech) return -1;
	ast_log(LOG_NOTICE, "(%s) Activate grammar: %s (not directly applicable)\n", google_speech->name, grammar_name);
	return 0;
}

/** \brief Deactivate a loaded grammar */
static int google_recog_deactivate_grammar(struct ast_speech *speech, const char *grammar_name)
{
	google_speech_t *google_speech = (google_speech_t*)speech->data;
	if (!google_speech) return -1;
	ast_log(LOG_NOTICE, "(%s) Deactivate grammar: %s (not directly applicable)\n", google_speech->name, grammar_name);
	return 0;
}

/** \brief Write audio to the speech engine */
static int google_recog_write(struct ast_speech *speech, void *data, int len)
{
	google_speech_t *google_speech = (google_speech_t*)speech->data;
	if (!google_speech || !google_speech->stream) {
		ast_log(LOG_WARNING, "(%s) Write called but stream not active.\n", google_speech ? google_speech->name : "unknown_google_speech");
		return -1;
	}

	try {
		google::cloud::speech::v1::StreamingRecognizeRequest request;
		request.set_audio_content(data, len);

		auto stream_ptr = static_cast<std::unique_ptr<grpc::ClientReaderWriter<google::cloud::speech::v1::StreamingRecognizeRequest, google::cloud::speech::v1::StreamingRecognizeResponse>>*>(google_speech->stream);

		// ast_mutex_lock(&speech->lock); // Protect stream writes if callbacks can be concurrent for the same speech object
		if (!(*stream_ptr)->Write(request)) {
			// ast_mutex_unlock(&speech->lock);
			ast_log(LOG_ERROR, "(%s) gRPC stream Write failed.\n", google_speech->name);
			// Stream is likely broken. Need to signal error and clean up.
			// Consider calling Finish() here or in a dedicated error handling path.
			ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY); // Or some error state
			return -1;
		}
		// ast_mutex_unlock(&speech->lock);

		// Simplified non-blocking read attempt.
		// A more robust solution uses a separate reader thread or CompletionQueue.
		// This example does a quick TryRead or equivalent if available, or a blocking Read
		// which might not be ideal if called directly from Asterisk core audio path.
		// For this iteration, we'll simulate a quick check.
		// grpc::CompletionQueue cq; // Would be part of a more complex async setup
		google::cloud::speech::v1::StreamingRecognizeResponse response;

		// This Read() call IS BLOCKING. A true non-blocking read or a read with timeout
		// requires more complex handling (e.g., ClientContext::set_deadline(), or a separate thread).
		// For simplicity in this step, we'll call Read and assume it returns if data is available or stream ends.
		// This is a known simplification point.
		if ((*stream_ptr)->Read(&response)) { // This can block!
			if (response.results_size() > 0) {
				const auto& result = response.results(0);
				if (result.alternatives_size() > 0) {
					const std::string& transcript = result.alternatives(0).transcript();
					ast_log(LOG_NOTICE, "(%s) Recognized: \"%s\" (is_final: %s, stability: %.2f)\n", google_speech->name, transcript.c_str(), result.is_final() ? "yes" : "no", result.stability());

					ast_free(google_speech->last_result);
					google_speech->last_result = ast_strdup(transcript.c_str());

					if (result.is_final()) {
						ast_speech_change_state(speech, AST_SPEECH_STATE_DONE);
						// Potentially signal Asterisk that a final result is ready via ast_speech_result_notify() if that's the pattern.
					}
				}
			}
		} // else: Read returned false (stream ended or error). Finish() in destroy will get status.

	} catch (const std::exception& e) {
		ast_log(LOG_ERROR, "(%s) Exception during google_recog_write: %s\n", google_speech->name, e.what());
		return -1;
	}
	return 0;
}

/** \brief Signal DTMF was received */
static int google_recog_dtmf(struct ast_speech *speech, const char *dtmf)
{
	google_speech_t *google_speech = (google_speech_t*)speech->data;
	if (!google_speech) return 0;
	ast_verb(4, "(%s) Signal DTMF %s (not typically used with STT)\n", google_speech->name, dtmf);
	return 0;
}

/** brief Prepare engine to accept audio */
static int google_recog_start(struct ast_speech *speech_obj) // Renamed from 'speech' to avoid conflict
{
	google_speech_t *google_speech = (google_speech_t*)speech_obj->data;
	if (!google_speech || !google_speech->speech_stub || !google_speech->context) {
		ast_log(LOG_ERROR, "Google Speech start called on incomplete setup.\n");
		return -1;
	}
	ast_debug(1, "(%s) Start recognition (establishing stream)\n", google_speech->name);

	// Establish the gRPC stream
	try {
		auto stub_ptr = static_cast<std::unique_ptr<google::cloud::speech::v1::Speech::StubInterface>*>(google_speech->speech_stub);
		auto context_ptr = static_cast<std::shared_ptr<grpc::ClientContext>*>(google_speech->context);

		// Release old stream if any (shouldn't happen if logic is correct)
		if (google_speech->stream) {
			ast_log(LOG_WARNING, "(%s) Stream already exists in start, cleaning up old one.\n", google_speech->name);
			delete static_cast<std::unique_ptr<grpc::ClientReaderWriter<google::cloud::speech::v1::StreamingRecognizeRequest, google::cloud::speech::v1::StreamingRecognizeResponse>>*>(google_speech->stream);
		}

		google_speech->stream = new auto((*stub_ptr)->StreamingRecognize(context_ptr->get()));

		if (!google_speech->stream) {
			ast_log(LOG_ERROR, "(%s) Failed to establish gRPC StreamingRecognize.\n", google_speech->name);
			return -1;
		}

		// Send initial configuration message
		google::cloud::speech::v1::StreamingRecognizeRequest request;
		auto* streaming_config = request.mutable_streaming_config();
		auto* recognition_config = streaming_config->mutable_config();

		recognition_config->set_encoding(google::cloud::speech::v1::RecognitionConfig::LINEAR16); // Assuming SLIN
		recognition_config->set_sample_rate_hertz(google_speech->sample_rate_hertz);
		recognition_config->set_language_code(google_speech->language_code);
		if (google_speech->model && google_speech->model[0] != '\0' && strcmp(google_speech->model, "default") != 0) {
			recognition_config->set_model(google_speech->model);
		}
		recognition_config->set_enable_automatic_punctuation(google_speech->enable_automatic_punctuation);

		streaming_config->set_interim_results(true); // Example: enable interim results
		// TODO: Add other config options like max_alternatives from speech object

		auto stream_rw_ptr = static_cast<std::unique_ptr<grpc::ClientReaderWriter<google::cloud::speech::v1::StreamingRecognizeRequest, google::cloud::speech::v1::StreamingRecognizeResponse>>*>(google_speech->stream);

		if (!(*stream_rw_ptr)->Write(request)) {
			ast_log(LOG_ERROR, "(%s) Failed to send initial config on gRPC stream.\n", google_speech->name);
			(*stream_rw_ptr)->Finish(); // Try to close gracefully
			delete stream_rw_ptr;
			google_speech->stream = NULL;
			return -1;
		}

	} catch (const std::exception& e) {
		ast_log(LOG_ERROR, "(%s) Exception during google_recog_start: %s\n", google_speech->name, e.what());
		if (google_speech->stream) { // Clean up partially created stream
			delete static_cast<std::unique_ptr<grpc::ClientReaderWriter<google::cloud::speech::v1::StreamingRecognizeRequest, google::cloud::speech::v1::StreamingRecognizeResponse>>*>(google_speech->stream);
			google_speech->stream = NULL;
		}
		return -1;
	}

	ast_speech_change_state(speech_obj, AST_SPEECH_STATE_READY);
	return 0;
}

/** \brief Change an engine specific setting */
static int google_recog_change(struct ast_speech *speech, const char *name, const char *value)
{
	google_speech_t *google_speech = (google_speech_t*)speech->data;
	if (!google_speech) return -1;
	ast_debug(1, "(%s) Change setting name: %s value:%s (placeholder)\n", google_speech->name, name, value);
	// This could be used to update language_code, etc., but would require re-init or care if stream is active.
	return 0;
}

/** \brief Get an engine specific attribute */
static int google_recog_get_settings(struct ast_speech *speech, const char *name, char *buf, size_t len)
{
	google_speech_t *google_speech = (google_speech_t*)speech->data;
	if (!google_speech) return -1;
	ast_debug(1, "(%s) Get settings name: %s (placeholder)\n", google_speech->name, name);
	return -1; // Indicate setting not found or not applicable
}

/** \brief Change the type of results we want back */
static int google_recog_change_results_type(struct ast_speech *speech, enum ast_speech_results_type results_type)
{
	google_speech_t *google_speech = (google_speech_t*)speech->data;
	if (!google_speech) return -1;
	ast_debug(1, "(%s) Change results type to %d (placeholder)\n", google_speech->name, results_type);
	// Could map to interim_results or other flags if applicable.
	return -1;
}

/** \brief Try to get result */
struct ast_speech_result* google_recog_get(struct ast_speech *speech)
{
	google_speech_t *google_speech = (google_speech_t*)speech->data;
	if (!google_speech) return NULL;

	// This function would typically be called after write() indicated a result,
	// or in a separate thread that reads from the gRPC stream.
	// For now, a simple placeholder that uses last_result if set.
	// A real implementation needs to handle blocking/non-blocking reads from the stream.

	if (ast_strlen_zero(google_speech->last_result)) {
		// ast_debug(1, "(%s) No new result available in google_recog_get.\n", google_speech->name);
		return NULL; // No new result
	}

	ast_log(LOG_DEBUG, "(%s) google_recog_get retrieving result: '%s'\n", google_speech->name, google_speech->last_result);

	struct ast_speech_result *speech_result;
	speech_result = (ast_speech_result*)ast_calloc(1, sizeof(struct ast_speech_result));
	if (!speech_result) return NULL;

	speech_result->text = google_speech->last_result; // Transfer ownership of the string
	google_speech->last_result = NULL;                // Mark as consumed
	speech_result->score = 100; // Google Speech API provides confidence scores per alternative.
	                                  // This could be populated from response.results(0).alternatives(0).confidence()

	ast_debug(1, "(%s) Returning result: '%s'\n", google_speech->name, speech_result->text);

	// Asterisk core usually sets this flag after a successful get().
	// ast_set_flag(speech, AST_SPEECH_HAVE_RESULTS);

	return speech_result;
}

/** \brief Speech engine declaration */
static struct ast_speech_engine ast_google_engine_cb = {
	.name = (char*)GOOGLE_ENGINE_NAME, // Cast needed for older Asterisk versions
	.create = google_recog_create,
	.destroy = google_recog_destroy,
	.load_grammar = google_recog_load_grammar,
	.unload_grammar = google_recog_unload_grammar,
	.activate_grammar = google_recog_activate_grammar,
	.deactivate_grammar = google_recog_deactivate_grammar,
	.write = google_recog_write,
	.dtmf = google_recog_dtmf,
	.start = google_recog_start,
	.stop = google_recog_stop, // Added stop
	.change = google_recog_change,
	.get_settings = google_recog_get_settings,
	.change_results_type = google_recog_change_results_type,
	.get = google_recog_get
};

/** \brief Load Google Speech engine configuration (/etc/asterisk/res_speech_google.conf)*/
static int google_engine_config_load(void)
{
	const char *value = NULL;
	struct ast_flags config_flags = { 0 }; // Initialize to empty
	struct ast_config *cfg = ast_config_load(GOOGLE_ENGINE_CONFIG, config_flags);

	// Set defaults first
	google_engine.default_language_code = ast_strdup("en-US");
	google_engine.default_service_account_key_path = NULL;
	google_engine.default_model = ast_strdup("default");
	google_engine.default_enable_automatic_punctuation = false;


	if (!cfg) {
		ast_log(LOG_WARNING, "No such configuration file %s. Using defaults.\n", GOOGLE_ENGINE_CONFIG);
	} else {
		// Service Account Key Path (optional)
		value = ast_variable_retrieve(cfg, "general", "service_account_key_path");
		if (!ast_strlen_zero(value)) {
			ast_free(google_engine.default_service_account_key_path); // Free default if overridden
			google_engine.default_service_account_key_path = ast_strdup(value);
		}

		// Language Code
		value = ast_variable_retrieve(cfg, "general", "language_code");
		if (!ast_strlen_zero(value)) {
			ast_free(google_engine.default_language_code); // Free default if overridden
			google_engine.default_language_code = ast_strdup(value);
		}

		// Model
		value = ast_variable_retrieve(cfg, "general", "model");
		if (!ast_strlen_zero(value)) {
			ast_free(google_engine.default_model); // Free default if overridden
			google_engine.default_model = ast_strdup(value);
		}

		// Enable Automatic Punctuation
		value = ast_variable_retrieve(cfg, "general", "enable_automatic_punctuation");
		if (!ast_strlen_zero(value)) {
			google_engine.default_enable_automatic_punctuation = ast_true(value);
		}
		ast_config_destroy(cfg);
	}

	ast_log(LOG_DEBUG, "Google Speech Engine Config: Lang=%s, KeyPath=%s, Model=%s, Punctuation=%s\n",
		google_engine.default_language_code,
		google_engine.default_service_account_key_path ? google_engine.default_service_account_key_path : "none",
		google_engine.default_model,
		google_engine.default_enable_automatic_punctuation ? "true" : "false");

	google_engine.initialized = 1;
	ast_log(LOG_NOTICE, "Google Speech engine configuration loaded.\n");
	return 0;
}

/** \brief Load module */
static int load_module(void)
{
	ast_log(LOG_NOTICE, "Loading Google Speech Recognition Engine module (res_speech_google)\n");

	/* Load engine configuration */
	if (google_engine_config_load() != 0) {
		// Error already logged by google_engine_config_load if it was critical
		return AST_MODULE_LOAD_DECLINE;
	}

	ast_google_engine_cb.formats = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if (!ast_google_engine_cb.formats) {
		ast_log(LOG_ERROR, "Failed to alloc media format capabilities for Google Speech engine.\n");
		return AST_MODULE_LOAD_FAILURE;
	}
	// Google Speech API supports various sample rates and encodings.
	// LINEAR16 is common. Add more formats if your Asterisk setup uses them and they are supported.
	ast_format_cap_append_by_type(ast_google_engine_cb.formats, AST_MEDIA_TYPE_AUDIO, ast_format_slin); // Signed Linear 16-bit, any rate
	// ast_format_cap_append(ast_google_engine_cb.formats, ast_format_slin16, 0); // SLIN at 16kHz
	// ast_format_cap_append(ast_google_engine_cb.formats, ast_format_slin8, 0);  // SLIN at 8kHz
	// etc.

	if (ast_speech_register(&ast_google_engine_cb)) {
		ast_log(LOG_ERROR, "Failed to register Google Speech engine.\n");
		ast_format_cap_destroy(ast_google_engine_cb.formats);
		return AST_MODULE_LOAD_FAILURE;
	}

	ast_log(LOG_NOTICE, "Google Speech engine loaded successfully.\n");
	return AST_MODULE_LOAD_SUCCESS;
}

/** \brief Unload module */
static int unload_module(void)
{
	ast_log(LOG_NOTICE, "Unloading Google Speech Recognition Engine module (res_speech_google)\n");
	if (ast_speech_unregister(GOOGLE_ENGINE_NAME)) {
		ast_log(LOG_ERROR, "Failed to unregister Google Speech engine.\n");
		// Proceed with freeing resources anyway
	}

	ast_format_cap_destroy(ast_google_engine_cb.formats);
	ast_free(google_engine.default_service_account_key_path);
	ast_free(google_engine.default_language_code);
	ast_free(google_engine.default_model);
	google_engine.initialized = 0;

	ast_log(LOG_NOTICE, "Google Speech engine unloaded.\n");
	return 0;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Google Speech Recognition Engine");
