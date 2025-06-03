# Asterisk Google Cloud Speech-to-Text Module

This repository provides an Asterisk external speech recognition module for integrating with Google Cloud's Speech-to-Text API.

## 1. Google Cloud Speech-to-Text (`res_speech_google`)

### 1.1. Introduction

The `res-speech-google` module integrates Asterisk with Google Cloud Speech-to-Text, allowing you to use Google's powerful speech recognition capabilities directly within your dialplan.

### 1.2. Dependencies

To build and run the `res-speech-google` module, you'll need the following:

*   **Build-time & Runtime:**
    *   **gRPC (C++)**: For communication with Google Cloud services.
    *   **Protocol Buffers (C++)**: For data serialization.
    *   **Google Cloud C++ Client Libraries (Speech component)**: Provides the C++ interface to the Google Cloud Speech-to-Text API.
    *   **OpenSSL**: Often a transitive dependency of gRPC and Google Cloud libraries, used for secure communication.
    *   **libcurl**: May be required by Google Cloud C++ libraries for HTTP communication (e.g., metadata server).
    *   **zlib**: Common compression library, often a dependency.

*   **Installation Guidance:**
    *   The easiest way to install these is often through your system's package manager. Package names can vary. For Debian/Ubuntu, you might use:
        ```bash
        sudo apt-get update
        sudo apt-get install libgrpc-dev libgrpc++-dev protobuf-compiler libprotobuf-dev \
                             libgoogle-cloud-cpp-speech-dev libssl-dev libcurl4-openssl-dev zlib1g-dev
        ```
    *   For other distributions (e.g., CentOS, Fedora), use `yum` or `dnf` and search for similar package names (e.g., `grpc-devel`, `protobuf-devel`).
    *   **Important:** The `google-cloud-cpp-speech-dev` (or similarly named) package is crucial. If it's not available in your distribution, you may need to compile the [Google Cloud C++ Client Libraries](https://github.com/googleapis/google-cloud-cpp) from source. Ensure the Speech component is enabled during its build.
    *   The `pkg-config` tool is used by the build system to find these libraries. Ensure your `.pc` files for these libraries are in a standard location.

### 1.3. Configuration (`conf/res_speech_google.conf`)

The behavior of the `res-speech-google` module is controlled by `conf/res_speech_google.conf`, which should be placed in your Asterisk configuration directory (e.g., `/etc/asterisk/`).

```ini
[general]
; Google Speech settings

; service_account_key_path = /path/to/your/service_account_key.json
; Optional: Path to the Google Cloud service account key JSON file.
; If commented out or not set, the module will attempt to use Application Default Credentials (ADC).
; ADC are typically configured by setting the GOOGLE_APPLICATION_CREDENTIALS environment variable
; or by running `gcloud auth application-default login`.
; How to obtain a service account key:
; 1. Go to the Google Cloud Console (console.cloud.google.com).
; 2. Ensure the "Cloud Speech-to-Text API" is enabled for your project.
; 3. Navigate to "IAM & Admin" -> "Service Accounts".
; 4. Select your project.
; 5. Click "+ CREATE SERVICE ACCOUNT".
; 6. Fill in the service account details.
; 7. Grant the "Cloud Speech-to-Text API User" role (or a role with equivalent permissions, like "Cloud AI Service Agent") to the service account.
; 8. Click "DONE".
; 9. Find the created service account in the list, click on the three dots under "Actions", and select "Manage keys".
; 10. Click "ADD KEY" -> "Create new key".
; 11. Choose "JSON" as the key type and click "CREATE". The JSON key file will be downloaded.
; 12. Securely store this file and provide its path here if you choose to use key-based authentication.

language_code = en-US
; Required: Language code for speech recognition (e.g., en-US, en-GB, es-ES, fr-FR).
; See https://cloud.google.com/speech-to-text/docs/languages for supported languages.

model = default
; Optional: Specifies the recognition model to use.
; Some common models include:
; - default: The default model for the language. Usually a good starting point.
; - phone_call: Best for audio that originated from a phone call (typically 8kHz or 16kHz).
; - command_and_search: Best for short queries and voice commands.
; - latest_long: Best for long-form audio (like voice mail). Consider for enhanced accuracy on longer inputs.
; - latest_short: Best for short audio clips.
; - medical_dictation: For spoken medical terms (US English only, requires access approval).
; - medical_conversation: For conversations between doctors and patients (US English only, requires access approval).
; If commented out or not set, "default" will be used.
; See https://cloud.google.com/speech-to-text/docs/speech-to-text-models for more details.

enable_automatic_punctuation = false
; Optional: If true, adds punctuation to recognition results.
; If commented out or not set, this defaults to false.
```

### 1.4. Building the Google Speech Module

Follow the general build instructions in Section 2. The `res-speech-google` module will be compiled if the `./configure` script successfully finds the gRPC and Google Cloud C++ Speech libraries. If these libraries are not found, a warning will be issued, and the module will not be built.

### 1.5. Usage in Asterisk

1.  **Load the module:**
    Ensure `res_speech_google.so` is loaded by Asterisk. You can add `load = res_speech_google.so` to your `modules.conf` or load it manually via the Asterisk CLI:
    ```
    asterisk*CLI> module load res_speech_google.so
    ```

2.  **Dialplan Example:**
    Use the `Speech()` application in your `extensions.conf` and specify `google` as the engine name.

    ```
    exten => googletest,1,NoOp(Starting Google Speech Test)
     same => n,Answer()
     same => n,Wait(1)
     same => n,SpeechCreate() ; Create a speech resource object
     same => n,SpeechStart()  ; Initialize the speech engine (starts gRPC stream)
     same => n,SayAlpha("Please say something after the beep.") ; Example prompt
    ; Record audio and send it for recognition.
    ; The 'google' engine will be used. 'en-US' is passed as the language.
    ; 'interim' can be used to get interim results if supported/desired by the application.
     same => n,SpeechBackground(beep,5) ; Record for up to 5 seconds after playing 'beep'
     same => n,NoOp(Recognition completed)
     same => n,Verbose(1, --- Transcription: ${SPEECH_TEXT(0)})
     same => n,Verbose(1, --- Speech Status: ${SPEECH_STATUS})
     same => n,Verbose(1, --- Speech Engine: ${SPEECH_ENGINE})
     same => n,SpeechDestroy() ; Clean up the speech resource
     same => n,Hangup()
    ```

    *   `Speech(variablename,engine[,language[,options]])`:
        *   `variablename`: The channel variable to store the transcription result (e.g., `myresult`).
        *   `engine`: Set to `google` for this module.
        *   `language`: (Optional) Language code like `en-US`, `es-ES`. If not provided, the `language_code` from `res_speech_google.conf` is used.
        *   `options`: (Optional) Comma-separated options. `interim` can be passed to request interim results.
    *   The final transcription is available in the channel variable you specified (e.g., `${SPEECH_TEXT(0)}` if `SpeechBackground` was used, or `${variablename}` if `Speech()` was used directly).
    *   `SPEECH_STATUS`: Indicates the outcome of the speech recognition (e.g., `OK`, `ERROR`, `TIMEOUT`).

### 1.6. Current Status/Limitations

*   The current implementation uses a blocking read for gRPC responses within the audio `write` callback. While functional, for very high-volume concurrent calls, this could introduce latency. Future enhancements might move response handling to a separate thread.
*   Advanced features like word timings, speaker diarization, or detailed alternative mapping are not yet fully exposed through the Asterisk application but may be available in the underlying Google API.

## 2. Build and Installation

1.  **Bootstrap the build system:**
    ```bash
    ./bootstrap
    ```

2.  **Configure the build:**
    Point to your Asterisk source directory. The `--prefix` should typically match your Asterisk installation prefix.
    ```bash
    ./configure --with-asterisk=<path_to_asterisk_source> --prefix=<path_to_asterisk_installation>
    ```
    For example, if Asterisk source is in `/usr/src/asterisk` and Asterisk is installed in `/usr`:
    ```bash
    ./configure --with-asterisk=/usr/src/asterisk --prefix=/usr
    ```
    If Asterisk was installed from packages and development headers are in standard locations, you might only need:
    ```bash
    ./configure --prefix=/usr
    ```
    The configure script will check for necessary dependencies for the module.

3.  **Compile:**
    ```bash
    make
    ```

4.  **Install:**
    ```bash
    sudo make install
    ```
    This will install the `.so` module file into your Asterisk modules directory.

5.  **Load Modules in Asterisk:**
    Edit `modules.conf` to ensure the base `res_speech.so` and the speech engine module are loaded:
    ```ini
    load = res_speech.so      ; Core speech API resource
    load = res_speech_google.so ; For Google Cloud Speech-to-Text
    ```
    Alternatively, load them manually via the Asterisk CLI for testing.

6.  **Verify Module Loading:**
    In the Asterisk CLI:
    ```
    asterisk*CLI> module show like res_speech_google.so
    ```
    You should see `res_speech_google.so` listed and loaded.

## 3. Dialplan Usage (General)

Refer to the Asterisk documentation for `Speech()` and `SpeechBackground()` applications.
The general idea is:
1.  `SpeechCreate()`: Creates a speech object.
2.  `SpeechStart()`: Initializes the recognition process with the engine. This is particularly important for engines that require an active connection or stream to be established before audio is sent (like Google Speech).
3.  `SpeechBackground(prompt[,timeout])` or `Speech(varname,...)`: To perform recognition.
4.  Access results via channel variables like `${SPEECH_TEXT(0)}` or the variable specified in `Speech()`.
5.  `SpeechDestroy()`: Cleans up the speech object and any associated engine resources.

Always check the Asterisk log files for detailed messages from the speech modules, especially during setup and testing.

## 4. Testing the Google Speech Module

This section provides a guide for manually testing the `res-speech-google` module.

### 4.1. Prerequisites

*   **Asterisk Installed:** A working Asterisk installation.
*   **Module Compiled & Installed:** `res_speech_google.so` must be compiled (as per Section 2) and present in your Asterisk modules directory (e.g., `/usr/lib/asterisk/modules/`).
*   **Google Cloud Platform (GCP) Account:**
    *   A GCP project with billing enabled.
    *   The **Cloud Speech-to-Text API** must be enabled for your project.
*   **Credentials Configured:** You need to authenticate to the Google Cloud Speech-to-Text API. Choose one method:
    1.  **Service Account Key JSON File:**
        *   Create a service account in GCP, grant it the "Cloud Speech-to-Text API User" or "Cloud AI Service Agent" role.
        *   Download its JSON key file.
        *   Update `conf/res_speech_google.conf` with the correct `service_account_key_path`.
    2.  **Application Default Credentials (ADC):**
        *   If `service_account_key_path` is *not* set in the config file, the module will use ADC.
        *   Configure ADC in the environment where Asterisk runs, typically by:
            *   Setting the `GOOGLE_APPLICATION_CREDENTIALS` environment variable to the path of your service account key JSON file.
            *   Or, if running on Google Cloud infrastructure (like Compute Engine), ADC might be automatically available.
            *   Or, by running `gcloud auth application-default login` as the user Asterisk runs under (less common for server daemons).
*   **Module Configuration File:** `conf/res_speech_google.conf` must be present in your Asterisk configuration directory (e.g., `/etc/asterisk/`).

### 4.2. Configuration Check (`conf/res_speech_google.conf`)

Before testing, double-check your `res_speech_google.conf`:

*   `language_code`: Set to your desired language (e.g., `en-US`, `es-ES`).
*   `service_account_key_path`: If using a key file, ensure this path is correct and the file is readable by the user Asterisk runs as. If using ADC, this line should be commented out or the path left empty.
*   `model`: Set to `default` or a specific model like `phone_call` as needed.
*   `enable_automatic_punctuation`: Set to `true` or `false`.

### 4.3. Asterisk Module Loading

1.  **Start Asterisk.** (If already running, you might need to restart or just load the module).
2.  **Access Asterisk CLI:** `sudo asterisk -rvvv` (the `vvv` increases verbosity).
3.  **Load the module:**
    ```
    asterisk*CLI> module load res_speech_google.so
    ```
4.  **Verify loading:**
    ```
    asterisk*CLI> module show like res_speech_google.so
    ```
    You should see output similar to:
    ```
    Module                         Description                              Use Count  Status      Support Level
    res_speech_google.so           Google Speech Recognition Engine         0          Running              core
    1 modules loaded
    ```
5.  **Check Logs:** Review the Asterisk log files (e.g., `/var/log/asterisk/messages` or `/var/log/asterisk/full`) for any error messages related to `res_speech_google` during loading. Pay attention to messages about configuration parsing or initial credential checks.

### 4.4. Dialplan Example for Testing

Add the following context to your `extensions.conf`:

```
[google-speech-test]
exten => talk,1,NoOp(=== Starting Google Speech Test ===)
 same => n,Answer()
 same => n,Wait(1) ; Allow media path to establish
 same => n,Ringing() ; Optional: indicate to caller that processing is starting

; Using SayText for prompt, but a pre-recorded audio file with Playback() is often better.
 same => n,SayText(Please say something clearly after the beep.)

; Create the speech resource for this channel
 same => n,SpeechCreate()
 same => n,Verbose(1, Speech resource created)

; Start the speech recognition engine (initializes gRPC stream)
 same => n,SpeechStart()
 same => n,Verbose(1, Speech recognition started with Google)

; Use SpeechBackground to play a beep and then listen.
; 'mygoogleresult' is the variable where the result will be stored.
; 'google' specifies the engine.
; You can optionally pass a language code here to override the default from the config.
; e.g., SpeechBackground(mygoogleresult,google,es-ES)
 same => n,SpeechBackground(mygoogleresult,google)
 same => n,Verbose(1, SpeechBackground started, listening for speech...)

; For this test, we'll also record the audio to a file to verify what was sent.
; This is optional for normal operation.
; Records for up to 15 seconds of audio, or stops after 5 seconds of silence.
 same => n,Record(/tmp/gspeechtest-${UNIQUEID}.wav,5,15,k)
 same => n,Verbose(1, Recording finished or timed out)

; Stop the speech recognition explicitly. This signals WritesDone to Google.
 same => n,SpeechStop()
 same => n,Verbose(1, Speech recognition stopped)

; It might take a moment for the final results to arrive after stopping.
 same => n,Wait(1) ; Give a brief moment for final processing

; Output the results
 same => n,Verbose(1, ---- Final Google Speech Transcription: ${mygoogleresult})
 same => n,Verbose(1, ---- Speech Status: ${SPEECH_STATUS})
 same => n,Verbose(1, ---- Speech Engine Used: ${SPEECH_ENGINE})
 same => n,Verbose(1, ---- Speech Confidence: ${SPEECH_CONFIDENCE(0)}) ; If available

; Read back the result to the caller
 same => n,SayText(I think you said: ${mygoogleresult})

; Clean up the speech resource
 same => n,SpeechDestroy()
 same => n,Verbose(1, Speech resource destroyed)

 same => n,Hangup()
 same => n,NoOp(=== Google Speech Test Ended ===)
```
**Note on `SpeechBackground` vs. `Record`:**
The `SpeechBackground()` application itself handles listening for audio. The `Record()` application in this example is primarily for debugging, allowing you to capture the audio that Asterisk is processing and sending (or attempting to send) to the speech engine. In a typical scenario, you might not need `Record()` if `SpeechBackground()` or `Speech()` is sufficient for your ASR needs.

### 4.5. Making a Test Call

1.  **Reload Dialplan:** In the Asterisk CLI: `dialplan reload`
2.  **Make a call:**
    *   **From a SIP phone:** Dial the extension `talk` in the context `google-speech-test` (or however you've set up access to this context).
    *   **From the Asterisk CLI (if you have a console channel driver like `chan_alsa` or `chan_dahdi` configured and working for audio):**
        ```
        asterisk*CLI> console dial talk@google-speech-test
        ```
        (This method's viability depends heavily on your Asterisk setup for console audio.)

### 4.6. Verification Steps & Expected Output

While the test call is active and after it has completed:

1.  **Monitor Asterisk CLI:**
    *   Ensure verbosity is adequate: `core set verbose 3` (or higher, e.g., `core set verbose 5` for more detail).
    *   Look for log messages from `res-speech-google` similar to (timestamps and details may vary):
        *   `NOTICE[...] res_speech_google.c: ... Google Speech engine configuration loaded.`
        *   `DEBUG[...] res_speech_google.c: (google) Create speech resource. Lang: en-US, Rate: 8000Hz, Model: phone_call, Punct: true` (example with 8kHz)
        *   `DEBUG[...] res_speech_google.c: (google) Start recognition (establishing stream)`
        *   `NOTICE[...] res_speech_google.c: (google) Recognized: "hello world" (is_final: yes, stability: 0.90)`
        *   The `Verbose` messages from your dialplan:
            *   `---- Final Google Speech Transcription: hello world`
            *   `---- Speech Status: DONE` (or `OK` depending on Asterisk version and exact flow)
            *   `---- Speech Engine Used: google`
    *   Listen for the `SayText` application reading back your transcribed speech.

2.  **Check Audio Recording (if used):**
    *   Listen to `/tmp/gspeechtest-${UNIQUEID}.wav`. Does it contain your spoken audio clearly? This helps isolate issues between audio capture by Asterisk and the ASR engine.

3.  **Check for Errors in Logs/CLI:**
    *   **Authentication/Permission Errors:** Messages like "Failed to create Google Cloud Credentials", "Permission denied", or errors related to the service account key file.
    *   **gRPC/Connection Errors:** Messages indicating failure to connect to `speech.googleapis.com`, SSL handshake failures, or RPC errors.
    *   **API Errors from Google:** Google might return specific error messages if the audio format is unsupported, the language code is invalid, or quotas are exceeded. These should be logged.
    *   `SPEECH_STATUS` variable being `ERROR`.

### 4.7. Troubleshooting Tips

*   **Google Cloud Project:**
    *   Double-check that the **Cloud Speech-to-Text API** is **Enabled** in your GCP project.
    *   Verify that billing is active for the project.
*   **Credentials:**
    *   If using a key file: Is the path in `service_account_key_path` correct? Is the file readable by the Asterisk user? Is the JSON content valid?
    *   If using ADC: Is the `GOOGLE_APPLICATION_CREDENTIALS` environment variable set correctly for the Asterisk process? Or are ADC configured correctly for the Google Cloud environment?
*   **Network Connectivity:**
    *   Ensure your Asterisk server can reach `speech.googleapis.com` on port 443. Test with `curl -v https://speech.googleapis.com`.
*   **Asterisk Logs:**
    *   The primary Asterisk log file (e.g., `/var/log/asterisk/full` or `/var/log/asterisk/messages`) is your best friend. Look for detailed errors from `res_speech_google.c`.
    *   Increase CLI verbosity: `core set verbose 5` and `core set debug 5` for maximum detail during the test call.
*   **Audio Format:**
    *   Ensure the audio format being sent to the module (typically determined by the channel's native format) is supported by Google Speech-to-Text (e.g., LINEAR16 for SLIN). The module currently defaults to LINEAR16. Mismatched sample rates can also be an issue.
*   **Quotas:** Check your Google Cloud Speech-to-Text API quotas in the GCP console to ensure you haven't exceeded them.
