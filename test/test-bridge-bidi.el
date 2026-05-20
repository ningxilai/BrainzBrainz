;;; test-bridge-bidi.el --- Test bidirectional RPC (multi-instance + C++↔Emacs)  -*- lexical-binding: t; -*-

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
    (message "=== Bidirectional RPC Test Results ===")
    (dolist (l (reverse log))
      (message "%s" l))
    (message "--- %d passed, %d failed ---" pass fail)
    (if (> fail 0) (kill-emacs 1) (kill-emacs 0)))

  (condition-case err
      (progn
        ;; ============================================================
        ;; Test 1: Start app via multi-instance API
        ;; ============================================================
        (condition-case e
            (let ((rpc (emacs-stdio-jsonrpc-start-app "bridge-test" bin-path)))
              (if (and rpc (emacs-stdio-jsonrpc-app-running-p "bridge-test"))
                  (log-ok "start-app: bridge-test running")
                (log-fail "start-app: failed")))
          (error (log-fail (format "start-app threw: %S" e))))

        ;; ============================================================
        ;; Test 2: Call echo method via multi-instance API
        ;; NOTE: json-serialize requires vectors for JSON arrays,
        ;;       plists/alists for JSON objects.
        ;; ============================================================
        (condition-case e
            (let ((result (emacs-stdio-jsonrpc-call "bridge-test" "echo" ["hello"])))
              (if (equal result ["hello"])
                  (log-ok "call echo: returned [\"hello\"]")
                (log-fail (format "call echo: got %S" result))))
          (error (log-fail (format "call echo threw: %S" e))))

        ;; ============================================================
        ;; Test 3: Multi-instance: call echo on two separate apps
        ;; ============================================================
        (condition-case e
            (let* ((rpc2 (emacs-stdio-jsonrpc-start-app "bridge-2" bin-path))
                   (r1 (emacs-stdio-jsonrpc-call "bridge-test" "echo" ["from-app-1"]))
                   (r2 (emacs-stdio-jsonrpc-call "bridge-2" "echo" ["from-app-2"])))
              (if (and (equal r1 ["from-app-1"]) (equal r2 ["from-app-2"]))
                  (log-ok "multi-instance: two apps respond independently")
                (log-fail (format "multi-instance: r1=%S r2=%S" r1 r2)))
              (emacs-stdio-jsonrpc-stop-app "bridge-2"))
          (error (log-fail (format "multi-instance threw: %S" e))))

        ;; ============================================================
        ;; Test 4: Register Emacs method and have C++ call it back
        ;;         (bidirectional RPC: C++ call_emacs → Emacs)
        ;; ============================================================
        (condition-case e
            (let ((emacs-result nil))
              (emacs-stdio-jsonrpc-register-method
               "bridge-test" "add"
               (lambda (params)
                 ;; params is a vector [a b] from C++
                 (setq emacs-result (+ (aref params 0) (aref params 1)))
                 emacs-result))
              (let ((result (emacs-stdio-jsonrpc-call "bridge-test" "emacs_add" [3 4])))
                (if (equal result 7)
                    (log-ok "bidirectional RPC: C++ called Emacs add(3,4) → 7")
                  (log-fail (format "bidirectional RPC: got %S, expected 7" result)))))
          (error (log-fail (format "bidirectional RPC threw: %S" e))))

        ;; ============================================================
        ;; Test 5: List running apps
        ;; ============================================================
        (condition-case e
            (let ((apps (emacs-stdio-jsonrpc-list-apps)))
              (if (and (member "bridge-test" apps) (not (member "bridge-2" apps)))
                  (log-ok (format "list-apps: %S" apps))
                (log-fail (format "list-apps: %S (unexpected)" apps))))
          (error (log-fail (format "list-apps threw: %S" e))))

        ;; ============================================================
        ;; Test 6: Stop app
        ;; ============================================================
        (condition-case e
            (progn
              (emacs-stdio-jsonrpc-stop-app "bridge-test")
              (unless (emacs-stdio-jsonrpc-app-running-p "bridge-test")
                (log-ok "stop-app: bridge-test stopped"))
              (when (emacs-stdio-jsonrpc-app-running-p "bridge-test")
                (log-fail "stop-app: process still running")))
          (error (log-fail (format "stop-app threw: %S" e))))

        ;; ============================================================
        ;; Test 7: Stop bridge-2 (cleanup)
        ;; ============================================================
        (condition-case e
            (progn
              (emacs-stdio-jsonrpc-stop-app "bridge-2")
              (unless (emacs-stdio-jsonrpc-app-running-p "bridge-2")
                (log-ok "stop-app: bridge-2 stopped"))
              (when (emacs-stdio-jsonrpc-app-running-p "bridge-2")
                (log-fail "stop-app: bridge-2 still running")))
          (error (log-fail (format "stop-app bridge-2 threw: %S" e))))

        (conclude))

    (quit
     (message "Test interrupted!")
     (kill-emacs 2))
    (error
     (message "Test top-level error: %S" err)
     (kill-emacs 3))))
