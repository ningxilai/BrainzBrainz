;;; test-emacs-stdio-jsonrpc.el --- Test for emacs-stdio-jsonrpc.el  -*- lexical-binding: t; -*-

(require 'jsonrpc)
(require 'cl-lib)

(let* ((script-dir (file-name-directory (or load-file-name default-directory)))
       (project-root (expand-file-name ".." script-dir))
       (el-path (expand-file-name "emacs-stdio-jsonrpc.el" project-root))
       (bin-path (expand-file-name "build/bridge_test_server" project-root))
       (log '())
       (pass 0)
       (fail 0))

  (load el-path nil t)

  (defun log-ok (msg)
    (push (concat "PASS: " msg) log)
    (setq pass (1+ pass)))

  (defun log-fail (msg)
    (push (concat "FAIL: " msg) log)
    (setq fail (1+ fail)))

  (defun conclude ()
    (message "=== emacs-stdio-jsonrpc Test Results ===")
    (dolist (l (reverse log))
      (message "%s" l))
    (message "--- %d passed, %d failed ---" pass fail)
    (if (> fail 0) (kill-emacs 1) (kill-emacs 0)))

  (unless (file-exists-p bin-path)
    (error "bridge_test_server not found; please build first"))

  (condition-case err
      (progn
        ;; Test 1: Start app
        (condition-case e
            (let ((rpc (emacs-stdio-jsonrpc-start-app "jsonrpc-test" bin-path)))
              (if (and rpc (emacs-stdio-jsonrpc-app-running-p "jsonrpc-test"))
                  (log-ok "start-app: subprocess running")
                (log-fail "start-app: failed")))
          (error (log-fail (format "start-app threw: %S" e))))

        ;; Test 2: Call echo method
        (condition-case e
            (let ((result (emacs-stdio-jsonrpc-call "jsonrpc-test" "echo" ["hello"])))
              (if (equal result ["hello"])
                  (log-ok "call echo: returned [\"hello\"]")
                (log-fail (format "call echo: got %S" result))))
          (error (log-fail (format "call echo threw: %S" e))))

        ;; Test 3: Call add method
        (condition-case e
            (let ((result (emacs-stdio-jsonrpc-call "jsonrpc-test" "add" [3 4])))
              (if (= result 7)
                  (log-ok "call add(3,4) = 7")
                (log-fail (format "call add: got %S, expected 7" result))))
          (error (log-fail (format "call add threw: %S" e))))

        ;; Test 4: Unknown method signals error
        (condition-case e
            (progn
              (emacs-stdio-jsonrpc-call "jsonrpc-test" "nonexistent" nil)
              (log-fail "nonexistent method should have signalled error"))
          (jsonrpc-error
           (log-ok "unknown method signals jsonrpc-error"))
          (error
           (log-fail (format "unknown method threw unexpected: %S" e))))

        ;; Test 5: Send notification
        (condition-case e
            (let ((result (emacs-stdio-jsonrpc-notify "jsonrpc-test" "ping" nil)))
              (if (null result)
                  (log-ok "notify ping: returned nil")
                (log-fail (format "notify ping: got %S" result))))
          (error (log-fail (format "notify ping threw: %S" e))))

        ;; Test 6: Register method (bidirectional RPC)
        (condition-case e
            (let ((emacs-result nil))
              (emacs-stdio-jsonrpc-register-method
               "jsonrpc-test" "multiply"
               (lambda (params)
                 (setq emacs-result (* (aref params 0) (aref params 1)))
                 emacs-result))
              (let ((result (emacs-stdio-jsonrpc-call "jsonrpc-test" "emacs_multiply" [7 6])))
                (if (equal result 42)
                    (log-ok "bidirectional RPC: emacs_multiply(7,6) = 42")
                  (log-fail (format "bidirectional RPC: got %S" result)))))
          (error (log-fail (format "bidirectional RPC threw: %S" e))))

        ;; Test 7: Unregister method
        (condition-case e
            (progn
              (emacs-stdio-jsonrpc-unregister-method "jsonrpc-test" "multiply")
              (condition-case e2
                  (progn
                    (emacs-stdio-jsonrpc-call "jsonrpc-test" "emacs_multiply" [2 3])
                    (log-fail "unregistered method should have failed"))
                (jsonrpc-error
                 (log-ok "unregister-method: emacs_multiply no longer callable"))
                (error
                 (log-fail (format "unregister-method: unexpected: %S" e2)))))
          (error (log-fail (format "unregister-method setup threw: %S" e))))

        ;; Test 8: List apps
        (condition-case e
            (let ((apps (emacs-stdio-jsonrpc-list-apps)))
              (if (member "jsonrpc-test" apps)
                  (log-ok (format "list-apps: %S" apps))
                (log-fail (format "list-apps: %S (missing jsonrpc-test)" apps))))
          (error (log-fail (format "list-apps threw: %S" e))))

        ;; Test 9: Stop app
        (condition-case e
            (progn
              (emacs-stdio-jsonrpc-stop-app "jsonrpc-test")
              (unless (emacs-stdio-jsonrpc-app-running-p "jsonrpc-test")
                (log-ok "stop-app: subprocess stopped"))
              (when (emacs-stdio-jsonrpc-app-running-p "jsonrpc-test")
                (log-fail "stop-app: process still running")))
          (error (log-fail (format "stop-app threw: %S" e))))

        (conclude))

    (quit
     (message "Test interrupted!")
     (kill-emacs 2))
    (error
     (message "Test top-level error: %S" err)
     (kill-emacs 3))))
