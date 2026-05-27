;;; emacs-stdio-jsonrpc.el --- JSON-RPC bridge for C++ subprocess  -*- lexical-binding: t; -*-

;; Copyright (C) 2025-2026

;; Author: emacs-stdio-jsonrpc contributors
;; URL: https://github.com/anomalyco/emacs-stdio-jsonrpc
;; Version: 0.3.0
;; Package-Requires: ((emacs "29.1") (jsonrpc "1.0"))
;; Keywords: comm, processes, json

;; SPDX-License-Identifier: MIT

;;; Commentary:

;; This package provides a client library for communicating with a C++
;; subprocess over JSON-RPC 2.0 using standard I/O with Content-Length
;; framing (LSP-compatible transport).
;;
;; It manages subprocess lifecycles and provides a clean API for
;; sending requests and receiving notifications.  Multiple independent
;; C++ subprocess instances are supported.  The library is designed
;; to work with Emacs' built-in `jsonrpc.el'.
;;
;; == API ==
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

;; --- State ---

(defvar emacs-stdio-jsonrpc--connections (make-hash-table :test 'equal)
  "Hash table of (app-name . jsonrpc-process-connection).")

(defvar emacs-stdio-jsonrpc--method-registry (make-hash-table :test 'equal)
  "Hash table of (app-name . method-hash-table).
Each method-hash-table maps method-name (symbol) to handler function.")

;; --- Internal Helpers ---

(defun emacs-stdio-jsonrpc--find-executable (name)
  "Find executable NAME by searching common locations."
  (let ((candidates (list name)))
    (when load-file-name
      (let* ((dir (file-name-directory load-file-name))
             (build (expand-file-name "build" dir)))
        (when (file-directory-p build)
          (nconc candidates (list (expand-file-name name build))))))
    (let ((cwd-build (expand-file-name (concat "build/" name))))
      (nconc candidates (list cwd-build)))
    (catch 'found
      (dolist (path candidates)
        (when (and (file-executable-p path)
                   (not (file-directory-p path)))
          (throw 'found path)))
      (executable-find name))))

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

(provide 'emacs-stdio-jsonrpc)
;;; emacs-stdio-jsonrpc.el ends here