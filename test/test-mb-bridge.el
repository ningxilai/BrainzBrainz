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

        ;; Test 5: Radiohead lookup carries tags/genres/sameAs
        (let* ((res (mb-bridge--call "lookup-artist"
                                     '(:id "a74b1b7f-71a5-4011-9441-d0b5e4122711")))
               (tags (plist-get res :tags))
               (genres (plist-get res :genres))
               (links (plist-get res :sameAs)))
          (if (and (> (seq-length tags) 0)
                   (> (seq-length genres) 0)
                   (> (seq-length links) 0)
                   (plist-get (aref tags 0) :name)
                   (plist-get (aref links 0) :url))
              (log-ok "tags/genres/sameAs present")
            (log-fail (format "tags/genres/sameAs missing: %S" res))))

        ;; Test 6: detail sections render headless
        (let ((artist '(:type "Person" :country "US" :sort-name "Davis, Miles"
                        :life-span (:begin "1926" :end "1991")
                        :tags [(:name "jazz" :count 5)]
                        :genres [(:name "jazz" :count 9)]
                        :sameAs [(:type "wikidata" :url "https://example.invalid/x")])) )
          (with-temp-buffer
            (mb--detail-artist artist)
            (if (and (string-match-p "Tags (1)" (buffer-string))
                     (string-match-p "Genres (1)" (buffer-string))
                     (string-match-p "Links (1)" (buffer-string))
                     (string-match-p "https://example.invalid/x" (buffer-string)))
                (log-ok "tags/genres/sameAs sections render")
              (log-fail (format "sections missing: %S" (buffer-string))))))

        ;; Test 7: search-label carries score (IMatch)
        (let* ((res (mb-bridge--call "search-label"
                                     '(:query "label:Warp" :limit 1 :offset 0)))
               (labels (plist-get res :labels))
               (l (aref labels 0)))
          (if (and (plist-get l :name)
                   (numberp (plist-get l :score)))
              (log-ok "search-label with score")
            (log-fail (format "search-label unexpected: %S" l))))

        ;; Test 8: lookup release-group
        (let ((res (mb-bridge--call "lookup-release-group"
                                    '(:id "b1392450-e666-3926-a536-22c65f834433"))))
          (if (and (equal (plist-get res :title) "OK Computer")
                   (equal (plist-get res :primary-type) "Album"))
              (log-ok "lookup release-group")
            (log-fail (format "release-group unexpected: %S" res))))

        ;; Test 9: browse releases by artist
        (let* ((res (mb-bridge--call "browse-release"
                                     '(:artist "a74b1b7f-71a5-4011-9441-d0b5e4122711"
                                       :limit 2 :offset 0)))
               (rels (plist-get res :releases)))
          (if (and (> (plist-get res :release-count) 100)
                   (> (seq-length rels) 0)
                   (plist-get (aref rels 0) :title))
              (log-ok "browse releases by artist")
            (log-fail (format "browse unexpected: %S" res))))

        ;; Test 10: browse rejects zero/two linked keys
        (condition-case e10
            (progn
              (mb-bridge--call "browse-release" '(:limit 1 :offset 0))
              (log-fail "browse without link did not error"))
          (error (log-ok "browse without link rejected")))

        ;; Test 11: generic detail fallback renders headless
        (with-temp-buffer
          (mb--detail-generic '(:name "Berlin" :type "City" :country "DE"
                                :iso-3166-1-codes ["DE"]
                                :life-span (:begin "1237" :end "")))
          (if (and (string-match-p "Berlin" (buffer-string))
                   (string-match-p "1237" (buffer-string)))
              (log-ok "generic detail fallback")
            (log-fail (format "generic detail unexpected: %S" (buffer-string)))))

        ;; Test 12: detail renderer runs headless
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
