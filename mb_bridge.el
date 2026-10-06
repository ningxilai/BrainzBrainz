;;; mb_bridge.el --- MusicBrainz frontend over mb_bridge JSON-RPC server  -*- lexical-binding: t; -*-

;; Copyright (C) 2026

;; Author: emacs-stdio-jsonrpc contributors
;; URL: https://github.com/anomalyco/emacs-stdio-jsonrpc
;; Version: 0.1.0
;; Package-Requires: ((emacs "29.1") (jsonrpc "1.0"))
;; Keywords: comm, processes, musicbrainz

;; SPDX-License-Identifier: MIT

;;; Commentary:

;; Emacs frontend for the `mb_bridge' MusicBrainz subprocess (see
;; src/mb_bridge.cpp).  Transport is stdio JSON-RPC via built-in
;; `jsonrpc.el'; results render in `tabulated-list-mode' buffers
;; (elpaca-manager style) and per-entity detail buffers.
;;
;; Information architecture follows BrainzWrap (search -> results list
;; with load-more -> entity detail pages), but depends only on
;; built-in libraries: no vui, no dash.
;;
;; Quick start:
;;   M-x mb-search           ; pick entity, enter query
;;   M-x mb-lookup           ; pick entity, enter MBID
;; In results: RET opens detail, + loads more, g refreshes, q quits.
;; In detail: RET on an MBID button looks it up.

;;; Code:

(require 'cl-lib)
(require 'jsonrpc)
(require 'seq)
(require 'subr-x)


;;; Connection

(defgroup mb-bridge nil
  "MusicBrainz client backed by the mb_bridge subprocess."
  :group 'external
  :prefix "mb-bridge-")

(defcustom mb-bridge-program
  (expand-file-name "build/mb_bridge"
                    (file-name-directory (or load-file-name buffer-file-name)))
  "Path to the mb_bridge server executable."
  :type 'file
  :group 'mb-bridge)

(defcustom mb-bridge-limit 10
  "Default number of results per search request."
  :type 'integer
  :group 'mb-bridge)

(defvar mb-bridge--connection nil
  "Active `jsonrpc-process-connection' to mb_bridge, or nil.")

(defun mb-bridge-start ()
  "Start the mb_bridge subprocess."
  (interactive)
  (when mb-bridge--connection
    (mb-bridge-stop))
  (unless (file-executable-p mb-bridge-program)
    (error "mb_bridge not found or not executable: %s (build first)" mb-bridge-program))
  (let ((proc (make-process :name "mb-bridge"
                            :command (list mb-bridge-program)
                            :coding 'binary
                            :connection-type 'pipe
                            :noquery t)))
    (set-process-query-on-exit-flag proc nil)
    (setq mb-bridge--connection
          (make-instance 'jsonrpc-process-connection
                         :name "mb-bridge"
                         :process proc)))
  (message "mb_bridge started"))

(defun mb-bridge-stop ()
  "Stop the mb_bridge subprocess."
  (interactive)
  (when mb-bridge--connection
    (ignore-errors
      (jsonrpc-notify mb-bridge--connection "exit" nil))
    (setq mb-bridge--connection nil))
  (message "mb_bridge stopped"))

(defun mb-bridge--call (method params)
  "Call METHOD on the bridge, starting it on demand."
  (unless (and mb-bridge--connection
               (jsonrpc-running-p mb-bridge--connection))
    (mb-bridge-start))
  (jsonrpc-request mb-bridge--connection method params))


;;; Small helpers (plist results: jsonrpc.el decodes to plists)

(defun mb--false (v)
  "Normalize json `:json-false' to nil."
  (if (eq v :json-false) nil v))

(defun mb--ms (ms)
  "Format milliseconds MS as m:ss."
  (if (numberp ms)
      (format "%d:%02d" (/ ms 60000) (/ (mod ms 60000) 1000))
    ""))

(defun mb--credit-string (entity)
  "Render ENTITY's artist-credit as \"name+join...\" string."
  (mapconcat (lambda (c)
               (concat (or (plist-get c :name) "")
                       (or (plist-get c :joinphrase) "")))
             (plist-get entity :artist-credit) ""))

(defun mb--entity-label (type)
  "Human label for ENTITY-TYPE string."
  (alist-get type '(("artist" . "Artist")
                    ("release" . "Release")
                    ("recording" . "Recording")
                    ("discid" . "Disc ID"))
             type nil #'equal))

;;; Per-entity summary lines (mirrors BrainzWrap format-*)

(defun mb--format-artist (a)
  (string-join
   (delq nil
         (list (plist-get a :name)
               (when-let* ((ty (plist-get a :type))) (format "[%s]" ty))
               (when-let* ((cc (plist-get a :country))) (format "(%s)" cc))))
   " "))

(defun mb--format-release (r)
  (format "%s%s%s%s"
          (or (plist-get r :title) "")
          (if-let* ((d (plist-get r :date))) (format " (%s)" d) "")
          (if-let* ((s (plist-get r :status))) (format " [%s]" s) "")
          (let ((ac (mb--credit-string r)))
            (if (string-empty-p ac) "" (format " — %s" ac)))))

(defun mb--format-recording (r)
  (format "%s%s%s"
          (or (plist-get r :title) "")
          (if-let* ((len (plist-get r :length))) (format " (%s)" (mb--ms len)) "")
          (let ((ac (mb--credit-string r)))
            (if (string-empty-p ac) "" (format " — %s" ac)))))


;;; Search results buffer (tabulated-list-mode, elpaca-manager style)

(defvar-local mb--entity nil "Entity type string for this results buffer.")
(defvar-local mb--query nil "Query string for this results buffer.")
(defvar-local mb--limit nil)
(defvar-local mb--offset nil)
(defvar-local mb--count nil)
(defvar-local mb--entries nil "Accumulated tabulated-list entries.")

(defvar mb-search-mode-map
  (let ((m (make-sparse-keymap)))
    (set-keymap-parent m tabulated-list-mode-map)
    (define-key m (kbd "RET") #'mb-show-at-point)
    (define-key m (kbd "+") #'mb-search-more)
    m)
  "Keymap for `mb-search-mode'.")

(define-derived-mode mb-search-mode tabulated-list-mode "MB-Search"
  "Major mode for MusicBrainz search results."
  :group 'mb-bridge
  (setq tabulated-list-format [("Summary" 70 nil)
                               ("Info" 22 nil)
                               ("MBID" 36 nil)])
  (tabulated-list-init-header))

(defun mb--entry-info (entity item)
  "Secondary column text for ITEM of ENTITY."
  (pcase entity
    ("artist" (or (plist-get item :country) ""))
    ("release" (string-join (delq nil (list (plist-get item :date)
                                            (plist-get item :status)))
                            " "))
    ("recording" (mb--ms (plist-get item :length)))
    (_ "")))

(defun mb--entry-summary (entity item)
  (pcase entity
    ("artist" (mb--format-artist item))
    ("release" (mb--format-release item))
    ("recording" (mb--format-recording item))
    (_ (or (plist-get item :title) (plist-get item :name) ""))))

(defun mb--list-key (entity)
  (intern (concat entity "s")))

(defun mb--make-entries (entity items)
  (mapcar (lambda (it)
            (list (or (plist-get it :id) "")
                  (vector (mb--entry-summary entity it)
                          (mb--entry-info entity it)
                          (or (plist-get it :id) ""))))
          items))

(defun mb--refresh-header ()
  (setq header-line-format
        (format " %s \"%s\" — %d of %s (RET detail, + more, g refresh, q quit)"
                (mb--entity-label mb--entity) mb--query
                (length mb--entries)
                (or mb--count "?"))))

(defun mb--run-search (entity query limit offset)
  (mb-bridge--call (concat "search-" entity)
                   (list :query query :limit limit :offset offset)))

(defun mb-search (entity query)
  "Search MusicBrainz ENTITY for QUERY, showing a results buffer."
  (interactive
   (list (completing-read "Entity: " '("artist" "release" "recording")
                           nil t nil nil "artist")
         (read-string "Query (e.g. artist:radiohead): ")))
  (let ((buf (get-buffer-create (format "*mb:%s:%s*" entity query))))
    (with-current-buffer buf
      (mb-search-mode)
      (setq mb--entity entity
            mb--query query
            mb--limit mb-bridge-limit
            mb--offset 0
            mb--entries nil)
      (message "Searching %s for %S..." entity query)
      (let* ((res (mb--run-search entity query mb-bridge-limit 0))
             (items (seq-into (plist-get res (mb--list-key entity)) 'list)))
        (setq mb--count (plist-get res :count)
              mb--entries (mb--make-entries entity items)
              tabulated-list-entries mb--entries)
        (tabulated-list-print t)
        (mb--refresh-header)))
    (pop-to-buffer buf)))

(defun mb-search-more ()
  "Load the next page of results into the current search buffer."
  (interactive nil mb-search-mode)
  (let ((next (+ mb--offset mb--limit)))
    (when (and mb--count (>= next mb--count))
      (user-error "No more results"))
    (message "Loading more...")
    (let* ((res (mb--run-search mb--entity mb--query mb--limit next))
           (items (seq-into (plist-get res (mb--list-key mb--entity)) 'list)))
      (setq mb--offset next
            mb--entries (append mb--entries
                                (mb--make-entries mb--entity items))
            tabulated-list-entries mb--entries)
      (tabulated-list-print t)
      (mb--refresh-header))))

(defun mb-show-at-point ()
  "Open a detail buffer for the result on the current line."
  (interactive nil mb-search-mode)
  (let ((id (tabulated-list-get-id)))
    (unless (and id (not (string-empty-p id)))
      (user-error "No MBID on this line"))
    (mb-lookup mb--entity id)))


;;; Detail buffers (special-mode, BrainzWrap section layout)

(defvar mb-detail-mode-map
  (let ((m (make-sparse-keymap)))
    (set-keymap-parent m special-mode-map)
    m)
  "Keymap for `mb-detail-mode'.")

(define-derived-mode mb-detail-mode special-mode "MB-Detail"
  "Major mode for a single MusicBrainz entity."
  :group 'mb-bridge)

(defun mb--meta (label value)
  (when (and value (not (equal value "")))
    (insert (propertize (format "%-14s " label) 'face 'bold)
            (format "%s\n" value))))

(defun mb--mbid-button (label entity id)
  "Insert LABEL text; RET on it looks up ENTITY/ID."
  (insert-text-button (or label id)
                      'action (lambda (_) (mb-lookup entity id))
                      'follow-link t
                      'help-echo (format "%s %s" entity id))
  (insert "\n"))

(defun mb--detail-artist (a)
  (mb--meta "Type" (plist-get a :type))
  (mb--meta "Country" (plist-get a :country))
  (mb--meta "Sort Name" (plist-get a :sort-name))
  (mb--meta "Disambiguation" (plist-get a :disambiguation))
  (when-let* ((ls (plist-get a :life-span)))
    (insert "\nLife Span\n")
    (mb--meta "Begin" (plist-get ls :begin))
    (mb--meta "End" (plist-get ls :end))))

(defun mb--detail-release (r)
  (mb--meta "Status" (plist-get r :status))
  (mb--meta "Date" (plist-get r :date))
  (mb--meta "Country" (plist-get r :country))
  (mb--meta "Barcode" (plist-get r :barcode))
  (mb--meta "Artists" (mb--credit-string r))
  (when-let* ((rg (plist-get r :release-group)))
    (mb--meta "Group" (format "%s [%s]"
                              (plist-get rg :title)
                              (plist-get rg :primary-type))))
  (when-let* ((labels (plist-get r :label-info)))
    (insert (format "\nLabels (%d)\n" (seq-length labels)))
    (seq-doseq (l (seq-into labels 'list))
      (when-let* ((lab (plist-get l :label)))
        (insert (format "- %s%s\n"
                        (plist-get lab :name)
                        (if-let* ((cat (plist-get l :catalog-number)))
                            (format " (%s)" cat)
                          ""))))))
  (when-let* ((media (plist-get r :media)))
    (seq-doseq (m (seq-into media 'list))
      (insert (format "\n[%s]\n" (or (plist-get m :format) "Medium")))
      (seq-doseq (tr (seq-into (plist-get m :tracks) 'list))
        (insert (format "  %2s. %-40s %s  "
                        (or (plist-get tr :number) "")
                        (or (plist-get tr :title) "")
                        (mb--ms (plist-get tr :length))))
        (when-let* ((rid (plist-get (plist-get tr :recording) :id)))
          (mb--mbid-button rid "recording" rid))))))

(defun mb--detail-recording (r)
  (mb--meta "Length" (mb--ms (plist-get r :length)))
  (mb--meta "Video" (if (mb--false (plist-get r :video)) "yes" "no"))
  (mb--meta "Artists" (mb--credit-string r))
  (when-let* ((isrcs (plist-get r :isrcs)))
    (mb--meta "ISRCs" (string-join (seq-into isrcs 'list) ", ")))
  (when-let* ((rels (plist-get r :releases)))
    (insert (format "\nReleases (%d)\n" (seq-length rels)))
    (seq-doseq (rel (seq-into rels 'list))
      (mb--mbid-button (plist-get rel :title) "release"
                       (plist-get rel :id)))))

(defun mb--detail-disc (d)
  (mb--meta "Sectors" (number-to-string (or (plist-get d :sectors) 0)))
  (when-let* ((rels (plist-get d :releases)))
    (insert (format "\nReleases (%d)\n" (seq-length rels)))
    (seq-doseq (rel (seq-into rels 'list))
      (mb--mbid-button (plist-get rel :title) "release"
                       (plist-get rel :id)))))

(defun mb-lookup (entity mbid)
  "Show a detail buffer for ENTITY (artist/release/recording/discid) MBID."
  (interactive
   (list (completing-read "Entity: " '("artist" "release" "recording" "discid")
                           nil t nil nil "artist")
         (read-string "MBID: ")))
  (message "Looking up %s %s..." entity mbid)
  (let* ((res (mb-bridge--call (concat "lookup-" entity)
                               (list :id mbid)))
         (buf (get-buffer-create (format "*mb:%s:%s*" entity mbid))))
    (with-current-buffer buf
      (let ((inhibit-read-only t))
        (erase-buffer)
        (insert (propertize (format "%s %s\n\n" (mb--entity-label entity) mbid)
                            'face 'bold))
        (pcase entity
          ("artist" (mb--detail-artist res))
          ("release" (mb--detail-release res))
          ("recording" (mb--detail-recording res))
          ("discid" (mb--detail-disc res)))
        (mb-detail-mode)
        (goto-char (point-min))))
    (pop-to-buffer buf)))

(provide 'mb_bridge)
;;; mb_bridge.el ends here
