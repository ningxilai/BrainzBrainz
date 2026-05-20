;;; emacs-stdio-jsonrpc.el --- JSON-RPC bridge for C++ subprocess  -*- lexical-binding: t; -*-

;; Copyright (C) 2025-2026

;; Author: emacs-stdio-jsonrpc contributors
;; URL: https://github.com/anomalyco/emacs-stdio-jsonrpc
;; Version: 0.2.0
;; Package-Requires: ((emacs "29.1") (jsonrpc "1.0"))
;; Keywords: comm, processes, json

;; SPDX-License-Identifier: MIT

;;; Commentary:

;; This package provides a client library for communicating with a C++
;; subprocess over JSON-RPC 2.0 using standard I/O with Content-Length
;; framing (LSP-compatible transport).
;;
;; It manages the subprocess lifecycle and provides a clean API for
;; sending requests and receiving notifications.  The library is designed
;; to work with Emacs' built-in `jsonrpc.el' library.
;;
;; == Multi-Instance API (v0.2.0) ==
;;
;;   ;; Start an application
;;   (emacs-stdio-jsonrpc-start-app "my-app" "/path/to/server")
;;
;;   ;; Call a method (request-response)
;;   (emacs-stdio-jsonrpc-call "my-app" "add" '(1 2))
;;   => 3
;;
;;   ;; Send a notification (no response)
;;   (emacs-stdio-jsonrpc-notify "my-app" "log" "hello")
;;
;;   ;; Register a method that C++ can call back into Emacs
;;   (emacs-stdio-jsonrpc-register-method "my-app" "get-buffer"
;;     (lambda (params) (buffer-name)))
;;
;;   ;; Stop an application
;;   (emacs-stdio-jsonrpc-stop-app "my-app")
;;
;; == Legacy API (unchanged) ==
;;
;;   (setq my-rpc (emacs-stdio-jsonrpc-start))
;;   (emacs-stdio-jsonrpc-process-feed xml-string)
;;   (emacs-stdio-jsonrpc-stop)
;;
;; == How Bidirectional RPC Works ==
;;
;; Emacs sends requests to C++ via the stdio pipe using jsonrpc.el's
;; standard `jsonrpc-request' / `jsonrpc-notify'.
;;
;; C++ sends requests to Emacs by writing to stdout a JSON-RPC message
;; with both "method" and "id" fields.  jsonrpc.el's process filter
;; detects this as a "remote request" (see `jsonrpc-connection-receive'
;; line 327) and dispatches to the :request-dispatcher handler.
;;
;; The :request-dispatcher looks up the method in a per-app hash table,
;; calls the registered Elisp function, and returns the result.
;; jsonrpc.el automatically sends the response back to C++.

;;; Code:

(require 'jsonrpc)
(require 'cl-lib)

;;;###autoload
(defgroup emacs-stdio-jsonrpc nil
  "JSON-RPC bridge for C++ subprocess."
  :prefix "emacs-stdio-jsonrpc-"
  :group 'comm)

(defcustom emacs-stdio-jsonrpc-executable "feed_processor"
  "Path to the feed_processor binary (default app)."
  :type 'string
  :group 'emacs-stdio-jsonrpc)

;; --- Multi-Instance State ---

(defvar emacs-stdio-jsonrpc--connections (make-hash-table :test 'equal)
  "Hash table of (app-name . jsonrpc-process-connection).")

(defvar emacs-stdio-jsonrpc--method-registry (make-hash-table :test 'equal)
  "Hash table of (app-name . method-hash-table).
Each method-hash-table maps method-name (symbol) to handler function.")

;; --- Legacy Global State ---

(defvar emacs-stdio-jsonrpc--connection nil
  "The jsonrpc-process-connection to the feed processor subprocess.")

(defvar emacs-stdio-jsonrpc--on-chunk-callback nil
  "Callback for processing feed_chunk notifications during a request.")

;; --- Internal Helpers ---

(defun emacs-stdio-jsonrpc--find-executable (&optional name)
  "Find executable NAME by searching common locations.
If NAME is nil, use `emacs-stdio-jsonrpc-executable'."
  (let* ((base (or name emacs-stdio-jsonrpc-executable))
         (candidates (list base)))
    (when load-file-name
      (let* ((dir (file-name-directory load-file-name))
             (build (expand-file-name "build" dir)))
        (when (file-directory-p build)
          (nconc candidates (list (expand-file-name base build))))))
    (let ((cwd-build (expand-file-name (concat "build/" base))))
      (nconc candidates (list cwd-build)))
    (catch 'found
      (dolist (path candidates)
        (when (and (file-executable-p path)
                   (not (file-directory-p path)))
          (throw 'found path)))
      (executable-find base))))

(defun emacs-stdio-jsonrpc--make-request-dispatcher (app-name)
  "Make a request-dispatcher function for APP-NAME.
Dispatches incoming C++ requests to methods registered in
`emacs-stdio-jsonrpc--method-registry'."
  (lambda (_conn method params)
    (let* ((methods (gethash app-name emacs-stdio-jsonrpc--method-registry))
           (handler (and methods (gethash method methods))))
      (if handler
          (funcall handler params)
        (signal 'jsonrpc-error
                (list :code -32601 :message (format "Method not found: %s" method)))))))

;; --- Multi-Instance API ---

;;;###autoload
(defun emacs-stdio-jsonrpc-start-app (app-name &optional executable command-args)
  "Start a C++ JSON-RPC subprocess named APP-NAME (a string).
If EXECUTABLE is nil, search for APP-NAME in build/ and PATH.
COMMAND-ARGS is an optional list of extra arguments passed to the
subprocess (e.g. database path).
Returns the jsonrpc-process-connection object."
  (interactive)
  (let* ((exe (emacs-stdio-jsonrpc--find-executable
               (or executable app-name)))
         (proc (make-process :name (format "jsonrpc-%s" app-name)
                              :command (cons exe command-args)
                              :coding 'binary
                              :connection-type 'pipe))
         (rpc (make-instance 'jsonrpc-process-connection
                             :name app-name
                             :process proc
                             :request-dispatcher
                             (emacs-stdio-jsonrpc--make-request-dispatcher app-name)
                             :events-buffer-config '(:size 0))))
    (puthash app-name rpc emacs-stdio-jsonrpc--connections)
    (unless (gethash app-name emacs-stdio-jsonrpc--method-registry)
      (puthash app-name (make-hash-table :test 'eq)
               emacs-stdio-jsonrpc--method-registry))
    rpc))

;;;###autoload
(defun emacs-stdio-jsonrpc-stop-app (app-name &optional kill)
  "Stop the C++ subprocess for APP-NAME.
If KILL is non-nil, kill the process immediately (SIGKILL)."
  (interactive)
  (let ((conn (gethash app-name emacs-stdio-jsonrpc--connections)))
    (when conn
      (if kill
          (delete-process (jsonrpc--process conn))
        (jsonrpc-notify conn "exit" nil)
        (let ((proc (jsonrpc--process conn)))
          (while (process-live-p proc)
            (sleep-for 0.1))))
      (remhash app-name emacs-stdio-jsonrpc--connections))))

;;;###autoload
(defun emacs-stdio-jsonrpc-app-running-p (app-name)
  "Return non-nil if APP-NAME's subprocess is running."
  (let ((conn (gethash app-name emacs-stdio-jsonrpc--connections)))
    (and conn (process-live-p (jsonrpc--process conn)))))

;;;###autoload
(cl-defun emacs-stdio-jsonrpc-call (app-name method params)
  "Call METHOD with PARAMS on APP-NAME (request-response).
APP-NAME is a string, METHOD is a string, PARAMS is a JSON-compatible value.
Returns the result from the C++ subprocess."
  (let ((conn (gethash app-name emacs-stdio-jsonrpc--connections)))
    (unless conn
      (error "App %s not started; call emacs-stdio-jsonrpc-start-app first" app-name))
    (jsonrpc-request conn method params)))

;;;###autoload
(cl-defun emacs-stdio-jsonrpc-notify (app-name method params)
  "Send notification METHOD with PARAMS to APP-NAME (no response)."
  (let ((conn (gethash app-name emacs-stdio-jsonrpc--connections)))
    (unless conn
      (error "App %s not started; call emacs-stdio-jsonrpc-start-app first" app-name))
    (jsonrpc-notify conn method params)))

;;;###autoload
(defun emacs-stdio-jsonrpc-register-method (app-name method handler)
  "Register HANDLER for METHOD that C++ can call on APP-NAME.
METHOD is a string, HANDLER is a function of (PARAMS).
When C++ calls `context.call_emacs(\"METHOD\", PARAMS)',
Emacs executes HANDLER and returns the result."
  (let* ((methods (gethash app-name emacs-stdio-jsonrpc--method-registry))
         (method-sym (if (stringp method) (intern method) method)))
    (unless methods
      (setq methods (make-hash-table :test 'eq))
      (puthash app-name methods emacs-stdio-jsonrpc--method-registry))
    (puthash method-sym handler methods)))

;;;###autoload
(defun emacs-stdio-jsonrpc-unregister-method (app-name method)
  "Unregister METHOD on APP-NAME."
  (let* ((methods (gethash app-name emacs-stdio-jsonrpc--method-registry))
         (method-sym (if (stringp method) (intern method) method)))
    (when methods
      (remhash method-sym methods))))

;;;###autoload
(defun emacs-stdio-jsonrpc-list-apps ()
  "Return list of running app names."
  (let ((live nil))
    (maphash (lambda (name conn)
               (when (process-live-p (jsonrpc--process conn))
                 (push name live)))
             emacs-stdio-jsonrpc--connections)
    (nreverse live)))

;; --- Legacy API (fully backward compatible) ---

;;;###autoload
(defun emacs-stdio-jsonrpc-start (&optional executable)
  "Start the feed processor subprocess.
If EXECUTABLE is nil, use `emacs-stdio-jsonrpc-executable'.
Returns the jsonrpc-process-connection object."
  (interactive)
  (let* ((exe (emacs-stdio-jsonrpc--find-executable
               (or executable emacs-stdio-jsonrpc-executable)))
         (proc (make-process :name "feed-processor"
                             :command (list exe)
                             :coding 'binary
                             :connection-type 'pipe))
         (rpc (make-instance 'jsonrpc-process-connection
                             :name "feed-processor"
                             :process proc
                             :request-dispatcher
                             (emacs-stdio-jsonrpc--make-request-dispatcher "feed-processor")
                             :events-buffer-config '(:size 0))))
    ;; Install notification dispatcher for feed_chunk
    (let ((orig (jsonrpc--notification-dispatcher rpc)))
      (setf (jsonrpc--notification-dispatcher rpc)
            (lambda (conn method params)
              (pcase method
                ('feed_chunk
                 (when emacs-stdio-jsonrpc--on-chunk-callback
                   (funcall emacs-stdio-jsonrpc--on-chunk-callback params))
                 (let* ((p (jsonrpc--process conn))
                        (chunks (process-get p :feed-chunks)))
                   (process-put p :feed-chunks (cons params chunks))))
                (_ (funcall orig conn method params))))))
    ;; Register in multi-instance table too
    (puthash "feed-processor" rpc emacs-stdio-jsonrpc--connections)
    (unless (gethash "feed-processor" emacs-stdio-jsonrpc--method-registry)
      (puthash "feed-processor" (make-hash-table :test 'eq)
               emacs-stdio-jsonrpc--method-registry))
    (setq emacs-stdio-jsonrpc--connection rpc)
    rpc))

;;;###autoload
(defun emacs-stdio-jsonrpc-stop (&optional kill)
  "Stop the feed processor subprocess.
If KILL is non-nil, kill the process immediately (SIGKILL)."
  (interactive)
  (let ((conn emacs-stdio-jsonrpc--connection))
    (when conn
      (if kill
          (delete-process (jsonrpc--process conn))
        (jsonrpc-notify conn "exit" nil)
        (let ((proc (jsonrpc--process conn)))
          (while (process-live-p proc)
            (sleep-for 0.1))))
      ;; Remove from both legacy and multi-instance state
      (remhash "feed-processor" emacs-stdio-jsonrpc--connections)
      (setq emacs-stdio-jsonrpc--connection nil))))

;;;###autoload
(defun emacs-stdio-jsonrpc-running-p ()
  "Return non-nil if the feed processor is running."
  (and emacs-stdio-jsonrpc--connection
       (process-live-p (jsonrpc--process emacs-stdio-jsonrpc--connection))))

;; Helper: extract value from JSON result (plist, alist, or hash-table).
(defun emacs-stdio-jsonrpc--result-get (result key)
  (cond ((hash-table-p result) (gethash key result))
        ((and (listp result) (keywordp (car result)))
         (plist-get result (intern (concat ":" key))))
        ((listp result)
         (alist-get key result))
        (t nil)))

;;;###autoload
(cl-defun emacs-stdio-jsonrpc-process-feed (xml &key chunk-size on-chunk)
  "Send XML to feed processor for parsing.
Returns a plist:
  :feed-title     - feed title string
  :total-items    - total item count
  :total-chunks   - total chunk count
  :chunks         - list of chunk plists (each has :chunk-index, :items, etc.)

ON-CHUNK is called with (PARAMS) for each feed_chunk notification."
  (unless emacs-stdio-jsonrpc--connection
    (error "feed processor not started; call emacs-stdio-jsonrpc-start first"))
  (let* ((conn emacs-stdio-jsonrpc--connection)
         (proc (jsonrpc--process conn)))
    ;; Record chunks for this request
    (process-put proc :feed-chunks nil)
    (let ((emacs-stdio-jsonrpc--on-chunk-callback on-chunk))
      (unwind-protect
          (let ((result (jsonrpc-request conn "process_feed"
                                         `((xml . ,xml)
                                           (chunk_size .
                                            ,(or chunk-size 10))))))
            ;; Drain pending timers so feed_chunk notifications arrive
            (dotimes (_ 20)
              (accept-process-output nil 0.01))
            (let ((chunks (nreverse (process-get proc :feed-chunks))))
              (list :feed-title (emacs-stdio-jsonrpc--result-get result "feed_title")
                    :total-items (emacs-stdio-jsonrpc--result-get result "total_items")
                    :total-chunks (emacs-stdio-jsonrpc--result-get result "total_chunks")
                    :chunks chunks)))
        (setq emacs-stdio-jsonrpc--on-chunk-callback nil)))))

;;;###autoload
(cl-defun emacs-stdio-jsonrpc-process-feeds (feeds &key chunk-size on-chunk on-feed-done)
  "Process a list of FEEDS through the subprocess.
FEEDS is a list of (LABEL . XML-STRING) conses.
ON-CHUNK is called as in `emacs-stdio-jsonrpc-process-feed'.
ON-FEED-DONE is called with (LABEL RESULT-PLIST) after each feed."
  (dolist (feed feeds)
    (let* ((label (car feed))
           (xml (cdr feed))
           (result (emacs-stdio-jsonrpc-process-feed xml
                     :chunk-size chunk-size
                     :on-chunk on-chunk)))
      (when on-feed-done
        (funcall on-feed-done label result)))))

;;;###autoload
(cl-defun emacs-stdio-jsonrpc-benchmark (feeds &key chunk-size (iterations 3))
  "Benchmark feed processing.
FEEDS: list of (LABEL . XML-STRING) pairs.
Returns list of (label . time-in-seconds) for each feed."
  (unless (emacs-stdio-jsonrpc-running-p)
    (emacs-stdio-jsonrpc-start))
  (let ((results nil))
    (dolist (feed feeds)
      (let* ((label (car feed))
             (xml (cdr feed))
             (times nil))
        (dotimes (_ iterations)
          (let ((start (float-time)))
            (emacs-stdio-jsonrpc-process-feed xml :chunk-size (or chunk-size 10))
            (push (- (float-time) start) times)))
        (push (cons label (/ (cl-reduce #'+ times) (length times))) results)))
    (nreverse results)))

(provide 'emacs-stdio-jsonrpc)
;;; emacs-stdio-jsonrpc.el ends here
