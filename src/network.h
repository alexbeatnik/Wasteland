#ifndef WASTELAND_NETWORK_H
#define WASTELAND_NETWORK_H

/* ---------------------------------------------------------------------------
 * Wasteland Network & Security Module
 *
 * Isolated libcurl download path and Linux seccomp lockdown.
 * --------------------------------------------------------------------------- */

/**
 * @brief Download a .gguf model from Hugging Face.
 *
 * @param model_id   Either a full https:// URL or a HuggingFace path such as
 *                   "TheBloke/Llama-2-7B-GGUF/resolve/main/model.gguf".
 * @param output_dir Directory where the file will be saved (e.g. "models").
 * @param progress   Pointer to an int 0-100 updated during transfer.
 * @param active     Pointer to an int set to 1 during transfer, 0 after.
 * @param cancel     Pointer to an int; if set to 1 download aborts.
 * @return 0 on success, -1 on failure/cancel.
 */
int network_download_model(const char *model_id, const char *output_dir,
                           int *progress, int *active, int *cancel);

/**
 * @brief Install a seccomp filter that kills the process if it ever attempts
 *        a socket-related syscall after lockdown.
 *
 * Once this returns successfully the process is physically incapable of
 * initiating or accepting network connections.
 *
 * @return 0 on success, -1 on failure.
 */
int lockdown_network(void);

#endif /* WASTELAND_NETWORK_H */
