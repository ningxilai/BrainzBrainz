;;; test-emacs-stdio-jsonrpc.el --- Test for emacs-stdio-jsonrpc.el  -*- lexical-binding: t; -*-

(require 'jsonrpc)

(let* ((script-dir (file-name-directory (or load-file-name default-directory)))
       (project-root (expand-file-name ".." script-dir))
       (el-path (expand-file-name "emacs-stdio-jsonrpc.el" project-root))
       (bin-path (expand-file-name "build/feed_processor" project-root))
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

  (defconst test-rss-xml
    "<?xml version=\"1.0\"?>
<rss version=\"2.0\">
  <channel>
    <title>Test Feed</title>
    <link>https://example.com</link>
    <description>Test</description>
    <item>
      <title>Item 1</title>
      <link>https://example.com/1</link>
      <description>First item</description>
      <pubDate>Mon, 18 May 2026 12:00:00 +0000</pubDate>
      <guid>guid-1</guid>
    </item>
    <item>
      <title>Item 2</title>
      <link>https://example.com/2</link>
      <description>Second item</description>
      <pubDate>Mon, 18 May 2026 13:00:00 +0000</pubDate>
      <guid>guid-2</guid>
    </item>
    <item>
      <title>Item 3</title>
      <link>https://example.com/3</link>
      <description>Third item</description>
      <pubDate>Mon, 18 May 2026 14:00:00 +0000</pubDate>
      <guid>guid-3</guid>
    </item>
  </channel>
</rss>")

  (condition-case err
      (progn
        ;; Test 1: Start subprocess
        (condition-case e
            (let ((rpc (emacs-stdio-jsonrpc-start bin-path)))
              (if (and rpc (emacs-stdio-jsonrpc-running-p))
                  (log-ok "start subprocess")
                (log-fail "start returned %S" rpc)))
          (error (log-fail (format "start threw: %S" e))))

        ;; Test 2: Process feed (chunk_size=10, all items in one chunk)
        (condition-case e
            (let* ((result (emacs-stdio-jsonrpc-process-feed
                            test-rss-xml :chunk-size 10))
                   (title (plist-get result :feed-title))
                   (items (plist-get result :total-items))
                   (chunks (plist-get result :total-chunks))
                   (chunk-list (plist-get result :chunks)))
              (if (and (equal title "Test Feed")
                       (= items 3)
                       (= chunks 1)
                       (= (length chunk-list) 1))
                    (log-ok (format "process-feed: title/items/chunks correct (%d items)" items))
                (log-fail (format "process-feed result: %S" result))))
          (error (log-fail (format "process-feed threw: %S" e))))

        ;; Test 3: Process feed with chunk_size=2, verify 2 chunks
        (condition-case e
            (let* ((result (emacs-stdio-jsonrpc-process-feed
                            test-rss-xml :chunk-size 2))
                   (chunks (plist-get result :total-chunks))
                   (chunk-list (plist-get result :chunks)))
              (if (and (= chunks 2) (= (length chunk-list) 2))
                  (log-ok "chunk_size=2 → 2 chunks")
                (log-fail (format "chunk_size=2 result: %S" result))))
          (error (log-fail (format "chunk_size=2 threw: %S" e))))

        ;; Test 4: on-chunk callback
        (condition-case e
            (let ((chunks-received nil))
              (emacs-stdio-jsonrpc-process-feed
               test-rss-xml :chunk-size 2
               :on-chunk (lambda (params)
                           (push params chunks-received)))
              (when (>= (length chunks-received) 2)
                (log-ok (format "on-chunk callback received %d chunks" (length chunks-received))))
              (unless (>= (length chunks-received) 2)
                (log-fail (format "on-chunk got %d chunks, expected ≥2"
                                  (length chunks-received)))))
          (error (log-fail (format "on-chunk threw: %S" e))))

        ;; Test 5: Batch process feeds
        (condition-case e
            (let* ((feeds `(("feed-a" . ,test-rss-xml)
                            ("feed-b" . ,test-rss-xml)))
                   (done-labels nil))
              (emacs-stdio-jsonrpc-process-feeds
               feeds :chunk-size 10
               :on-feed-done (lambda (label result)
                               (push label done-labels)
                               (unless (equal (plist-get result :feed-title) "Test Feed")
                                 (log-fail "batch: wrong title for %s" label))))
              (if (= (length done-labels) 2)
                  (log-ok (format "batch process 2 feeds: %S" done-labels))
                (log-fail (format "batch: only processed %d feeds" (length done-labels)))))
          (error (log-fail (format "batch threw: %S" e))))

        ;; Test 6: Benchmark
        (condition-case e
            (let ((bench (emacs-stdio-jsonrpc-benchmark
                          `(("test" . ,test-rss-xml)) :iterations 2)))
              (if (and (= (length bench) 1) (numberp (cdar bench)))
                  (log-ok (format "benchmark: %.4fs per iteration" (cdar bench)))
                (log-fail (format "benchmark returned %S" bench))))
          (error (log-fail (format "benchmark threw: %S" e))))

        ;; Test 7: Stop subprocess
        (condition-case e
            (progn
              (emacs-stdio-jsonrpc-stop)
              (unless (emacs-stdio-jsonrpc-running-p)
                (log-ok "stop subprocess"))
              (when (emacs-stdio-jsonrpc-running-p)
                (log-fail "process still running after stop")))
          (error (log-fail (format "stop threw: %S" e))))

        (conclude))

    (quit
     (message "Test interrupted!")
     (kill-emacs 2))
    (error
     (message "Test top-level error: %S" err)
     (kill-emacs 3))))
