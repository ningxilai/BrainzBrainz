;;; integration.el --- End-to-end test for jsonrpc.hpp with Emacs jsonrpc.el  -*- lexical-binding: t; -*-

(require 'jsonrpc)

(setq jsonrpc-debug nil)

(let* ((exe (expand-file-name
             (cond ((file-exists-p "../build/example_linux") "../build/example_linux")
                   ((file-exists-p "build/example_linux") "build/example_linux")
                   (t (error "example_linux not found; please build first")))))
       (log '())
       (pass 0)
       (fail 0))

  (defun log-ok (msg)
    (push (concat "PASS: " msg) log)
    (setq pass (1+ pass)))

  (defun log-fail (msg)
    (push (concat "FAIL: " msg) log)
    (setq fail (1+ fail)))

  (defun conclude ()
    (message "=== Integration Test Results ===")
    (dolist (l (reverse log))
      (message "%s" l))
    (message "--- %d passed, %d failed ---" pass fail)
    (if (> fail 0) (kill-emacs 1) (kill-emacs 0)))

  (condition-case err
      (let* ((proc (make-process :name "jrpc-test"
                                 :command `(,exe)
                                 :coding 'binary
                                 :connection-type 'pipe))
             (rpc (make-instance 'jsonrpc-process-connection
                                 :name "jrpc-integration"
                                 :process proc
                                 :events-buffer-config '(:size 0))))
        (message "Integration test: Emacs %s, subprocess PID %d"
                 emacs-version (process-id proc))

        ;; Test 1: sync request "add"
        (condition-case e1
            (let ((result (jsonrpc-request rpc "add" [3 4])))
              (if (= result 7.0)
                  (log-ok "sync add(3,4) = 7.0")
                (log-fail (format "sync add returned %S, expected 7.0" result))))
          (error (log-fail (format "sync add threw: %S" e1))))

        ;; Test 2: async "heavy_task" (3s delay)
        (condition-case e2
            (let ((result (jsonrpc-request rpc "heavy_task" nil)))
              (if (equal result "Task Complete!")
                  (log-ok "async heavy_task returned \"Task Complete!\"")
                (log-fail (format "async heavy_task returned %S" result))))
          (error (log-fail (format "async heavy_task threw: %S" e2))))

        ;; Test 3: unknown method should error
        (condition-case e3
            (progn
              (jsonrpc-request rpc "nonexistent" nil)
              (log-fail "nonexistent method should have signalled jsonrpc-error"))
          (jsonrpc-error
           (log-ok "unknown method signals jsonrpc-error"))
          (error
           (log-fail (format "unknown method threw unexpected: %S" e3))))

        ;; Test 4: notification "exit" → process should die cleanly
        (condition-case e4
            (let ((status (progn
                            (jsonrpc-notify rpc "exit" nil)
                            (while (process-live-p proc)
                              (sleep-for 0.1))
                            (process-exit-status proc))))
              (if (integerp status)
                  (log-ok (format "exit notification: process exited with code %d" status))
                (log-fail (format "exit notification: unexpected status %S" status))))
          (error (log-fail (format "exit notification threw: %S" e4))))

        (conclude))

    (quit
     (message "Test interrupted!")
     (kill-emacs 2))
    (error
     (message "Integration test top-level error: %S" err)
     (kill-emacs 3))))
