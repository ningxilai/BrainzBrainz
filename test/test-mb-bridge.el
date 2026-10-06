;;; test-mb-bridge.el --- Batch test for mb_bridge.el + mb_bridge server  -*- lexical-binding: t; -*-

;; Run: emacs -Q --batch -L .. -l test-mb-bridge.el
;; Requires built ../build/mb_bridge and network access to musicbrainz.org.

(require 'mb_bridge)

(setq jsonrpc-debug nil)

(let* ((exe (expand-file-name
             (cond ((file-exists-p "../build/mb_bridge") "../build/mb_bridge")
                   ((file-exists-p "build/mb_bridge") "build/mb_bridge")
                   (t (error "mb_bridge not found; please build first")))))
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
    (ignore-errors (mb-bridge-stop))
    (message "=== mb_bridge Test Results ===")
    (dolist (l (reverse log))
      (message "%s" l))
    (message "--- %d passed, %d failed ---" pass fail)
    (kill-emacs (if (> fail 0) 1 0)))

  (setq mb-bridge-program exe)

  (condition-case err
      (progn
        ;; Test 1: search-artist returns plists with kebab-case keys
        (let* ((res (mb-bridge--call "search-artist"
                                     '(:query "artist:radiohead" :limit 1 :offset 0)))
               (artists (plist-get res :artists))
               (a (aref artists 0)))
          (if (and (equal (plist-get a :name) "Radiohead")
                   (equal (plist-get a :country) "GB")
                   (equal (plist-get (plist-get a :life-span) :begin) "1991"))
              (log-ok "search-artist radiohead")
            (log-fail (format "search-artist unexpected: %S" a))))

        ;; Test 2: lookup-release has 12 tracks, nested recording
        (let* ((res (mb-bridge--call "lookup-release"
                                     '(:id "4b3d18cc-8937-36f4-8de0-481088be58e6")))
               (media (plist-get res :media))
               (tracks (plist-get (aref media 0) :tracks)))
          (if (and (equal (plist-get res :title) "OK Computer")
                   (= (length tracks) 12)
                   (equal (plist-get (aref tracks 0) :title) "Airbag"))
              (log-ok "lookup-release OK Computer, 12 tracks")
            (log-fail (format "lookup-release unexpected: %S" res))))

        ;; Test 3: bad inc rejected with -32602
        (condition-case e3
            (progn
              (mb-bridge--call "lookup-artist"
                               '(:id "a74b1b7f-71a5-4011-9441-d0b5e4122711"
                                 :inc "bogus-thing"))
              (log-fail "bad inc did not error"))
          (error
           (if (string-match-p "bogus-thing" (error-message-string e3))
               (log-ok "bad inc rejected")
             (log-fail (format "bad inc wrong error: %S" e3)))))

        ;; Test 4: tabulated-list entry builder (no network)
        (let ((ents (mb--make-entries
                     "artist"
                     (list '(:id "x" :name "N" :type "Person" :country "US")))))
          (if (and (= (length ents) 1)
                   (equal (car (car ents)) "x")
                   (equal (aref (cadr (car ents)) 0) "N [Person] (US)"))
              (log-ok "entry builder")
            (log-fail (format "entry builder unexpected: %S" ents))))

        ;; Test 5: detail renderer runs headless
        (with-temp-buffer
          (mb--detail-artist '(:type "Person" :country "US" :sort-name "Davis, Miles"
                               :disambiguation "" :life-span (:begin "1926" :end "1991")))
          (if (and (string-match-p "Sort Name" (buffer-string))
                   (string-match-p "Davis, Miles" (buffer-string))
                   (string-match-p "1926" (buffer-string)))
              (log-ok "detail renderer runs")
            (log-fail (format "detail unexpected: %S" (buffer-string)))))
        (conclude))
    (error
     (ignore-errors (mb-bridge-stop))
     (message "FATAL: %S" err)
     (kill-emacs 1))))
