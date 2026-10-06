;;; musicbrainz.el --- MusicBrainz frontend over musicbrainz JSON-RPC server  -*- lexical-binding: t; -*-

;; Copyright (C) 2026

;; Author: emacs-stdio-jsonrpc contributors
;; URL: https://github.com/anomalyco/emacs-stdio-jsonrpc
;; Version: 0.1.0
;; Package-Requires: ((emacs "29.1") (jsonrpc "1.0"))
;; Keywords: comm, processes, musicbrainz

;; SPDX-License-Identifier: MIT

;;; Commentary:

;; Emacs frontend for the `musicbrainz' MusicBrainz subprocess (see
;; src/musicbrainz.cpp).  Transport is stdio JSON-RPC via built-in
;; `jsonrpc.el'; results render in `tabulated-list-mode' buffers
;; (elpaca-manager style) and per-entity detail buffers.
;;
;; Information architecture follows BrainzWrap (search -> results list
;; with load-more -> entity detail pages), but depends only on
;; built-in libraries: no vui, no dash.
;;
;; Quick start:
;;   M-x musicbrainz-search           ; pick entity, enter query
;;   M-x musicbrainz-lookup           ; pick entity, enter MBID
;; In results: RET opens detail, + loads more, g refreshes, q quits.
;; In detail: RET on an MBID button looks it up.

;;; Code:

(require 'cl-lib)
(require 'jsonrpc)
(require 'seq)
(require 'subr-x)


;;; Connection

(defgroup musicbrainz nil
  "MusicBrainz client backed by the musicbrainz subprocess."
  :group 'external
  :prefix "musicbrainz-")

(defcustom musicbrainz-program
  (expand-file-name "build/musicbrainz"
                    (file-name-directory (or load-file-name buffer-file-name)))
  "Path to the musicbrainz server executable."
  :type 'file
  :group 'musicbrainz)

(defcustom musicbrainz-limit 10
  "Default number of results per search request."
  :type 'integer
  :group 'musicbrainz)

(defvar musicbrainz--connection nil
  "Active `jsonrpc-process-connection' to musicbrainz, or nil.")

(defun musicbrainz-start ()
  "Start the musicbrainz subprocess."
  (interactive)
  (when musicbrainz--connection
    (musicbrainz-stop))
  (unless (file-executable-p musicbrainz-program)
    (error "musicbrainz not found or not executable: %s (build first)" musicbrainz-program))
  (let ((proc (make-process :name "musicbrainz"
                            :command (list musicbrainz-program)
                            :coding 'binary
                            :connection-type 'pipe
                            :noquery t)))
    (set-process-query-on-exit-flag proc nil)
    (setq musicbrainz--connection
          (make-instance 'jsonrpc-process-connection
                         :name "musicbrainz"
                         :process proc)))
  (message "musicbrainz started"))

(defun musicbrainz-stop ()
  "Stop the musicbrainz subprocess."
  (interactive)
  (when musicbrainz--connection
    (ignore-errors
      (jsonrpc-notify musicbrainz--connection "exit" nil))
    (setq musicbrainz--connection nil))
  (message "musicbrainz stopped"))

(defun musicbrainz--call (method params)
  "Call METHOD on the bridge, starting it on demand."
  (unless (and musicbrainz--connection
               (jsonrpc-running-p musicbrainz--connection))
    (musicbrainz-start))
  (jsonrpc-request musicbrainz--connection method params))


;;; Small helpers (plist results: jsonrpc.el decodes to plists)

(defun musicbrainz--false (v)
  "Normalize json `:json-false' to nil."
  (if (eq v :json-false) nil v))

(defun musicbrainz--ms (ms)
  "Format milliseconds MS as m:ss."
  (if (numberp ms)
      (format "%d:%02d" (/ ms 60000) (/ (mod ms 60000) 1000))
    ""))

(defun musicbrainz--credit-string (entity)
  "Render ENTITY's artist-credit as \"name+join...\" string."
  (mapconcat (lambda (c)
               (concat (or (plist-get c :name) "")
                       (or (plist-get c :joinphrase) "")))
             (plist-get entity :artist-credit) ""))

(defun musicbrainz--entity-label (type)
  "Human label for ENTITY-TYPE string."
  (alist-get type '(("artist" . "Artist")
                    ("release" . "Release")
                    ("recording" . "Recording")
                    ("label" . "Label")
                    ("release-group" . "Release Group")
                    ("work" . "Work")
                    ("area" . "Area")
                    ("place" . "Place")
                    ("event" . "Event")
                    ("series" . "Series")
                    ("instrument" . "Instrument")
                    ("collection" . "Collection")
                    ("url" . "URL")
                    ("annotation" . "Annotation")
                    ("tag" . "Tag")
                    ("cdstub" . "CD Stub")
                    ("discid" . "Disc ID"))
             type nil #'equal))

;;; Entity registry: one entry per entity, mirroring the C++ AllEntities
;;; tuple and musicbrainz-api's per-entity overloads. All dispatch
;;; (search/lookup/browse/detail) goes through this table.

(defvar musicbrainz--entities
  ;; :links mirrors the C++ browse_links (BrowseXEntityParams in TS).
  '(("artist"       :search t :lookup t :browse t :list :artists
      :links ("area" "collection" "recording" "release" "release-group" "work"))
    ("release"      :search t :lookup t :browse t :list :releases
      :links ("area" "artist" "editor" "event" "label" "place" "recording" "release" "release-group" "track_artist" "work"))
    ("recording"    :search t :lookup t :browse t :list :recordings
      :links ("artist" "collection" "release" "work"))
    ("label"        :search t :lookup t :browse t :list :labels
      :links ("area" "collection" "release"))
    ("release-group" :search t :lookup t :browse t :list :release-groups
      :links ("artist" "collection" "release"))
    ("work"         :search t :lookup t :browse t :list :works
      :links ("artist" "collection"))
    ("area"         :search t :lookup t :browse t :list :areas
      :links ("collection"))
    ("place"        :search t :lookup t :browse t :list :places
      :links ("area" "collection"))
    ("event"        :search t :lookup t :browse t :list :events
      :links ("area" "artist" "collection" "place"))
    ("series"       :search t :lookup t :browse t :list :series
      :links ("collection"))
    ("instrument"  :search t :lookup t :browse t :list :instruments
      :links ("collection"))
    ("collection"   :search nil :lookup t :browse t :list :collections
      :links ("area" "artist" "editor" "event" "label" "place" "recording" "release" "release-group" "work"))
    ("url"          :search t :lookup t :browse t :list :urls
      :links ("resource"))
    ("annotation"   :search t :lookup nil :browse nil :list :annotations)
    ("tag"          :search t :lookup nil :browse nil :list :tags)
    ("cdstub"       :search t :lookup nil :browse nil :list :cdstubs)
    ("discid"       :search nil :lookup t :browse nil :list :releases))
  "Entity capability table. Keys mirror musicbrainz-api's method set:
searchable/lookable/browsable per entity; :list is the search key;
:links mirrors the C++ browse_links (TS BrowseXEntityParams).")

(defun musicbrainz--entities-where (prop)
  (mapcar #'car (seq-filter (lambda (e) (plist-get (cdr e) prop))
                            musicbrainz--entities)))

(defun musicbrainz--entity-prop (entity prop)
  (plist-get (cdr (assoc entity musicbrainz--entities)) prop))

;;; Per-entity summary lines (mirrors BrainzWrap format-*)

(defun musicbrainz--format-artist (a)
  (string-join
   (delq nil
         (list (plist-get a :name)
               (when-let* ((ty (plist-get a :type))) (format "[%s]" ty))
               (when-let* ((cc (plist-get a :country))) (format "(%s)" cc))))
   " "))

(defun musicbrainz--format-release (r)
  (format "%s%s%s%s"
          (or (plist-get r :title) "")
          (if-let* ((d (plist-get r :date))) (format " (%s)" d) "")
          (if-let* ((s (plist-get r :status))) (format " [%s]" s) "")
          (let ((ac (musicbrainz--credit-string r)))
            (if (string-empty-p ac) "" (format " — %s" ac)))))

(defun musicbrainz--format-recording (r)
  (format "%s%s%s"
          (or (plist-get r :title) "")
          (if-let* ((len (plist-get r :length))) (format " (%s)" (musicbrainz--ms len)) "")
          (let ((ac (musicbrainz--credit-string r)))
            (if (string-empty-p ac) "" (format " — %s" ac)))))

(defun musicbrainz--format-label (l)
  (string-join
   (delq nil
         (list (plist-get l :name)
               (when-let* ((ty (plist-get l :type))) (format "[%s]" ty))
               (when-let* ((code (plist-get l :label-code))) (format "(LC %s)" code))))
   " "))

(defun musicbrainz--format-release-group (g)
  (format "%s%s%s"
          (or (plist-get g :title) "")
          (if-let* ((d (plist-get g :first-release-date))) (format " (%s)" d) "")
          (if-let* ((p (plist-get g :primary-type))) (format " [%s]" p) "")))

(defun musicbrainz--format-work (w)
  (string-join
   (delq nil
         (list (plist-get w :title)
               (when-let* ((ty (plist-get w :type))) (format "[%s]" ty))
               (when-let* ((lang (plist-get w :language))) (format "(%s)" lang))))
   " "))

(defun musicbrainz--format-area (a)
  (string-join
   (delq nil
         (list (plist-get a :name)
               (when-let* ((ty (plist-get a :type))) (format "[%s]" ty))))
   " "))

(defun musicbrainz--format-place (p)
  (string-join
   (delq nil
         (list (plist-get p :name)
               (when-let* ((ty (plist-get p :type))) (format "[%s]" ty))
               (when-let* ((ad (plist-get p :address))) (format "(%s)" ad))))
   " "))

(defun musicbrainz--format-event (e)
  (string-join
   (delq nil
         (list (plist-get e :name)
               (when-let* ((ty (plist-get e :type))) (format "[%s]" ty))
               (when-let* ((tm (plist-get e :time))) (format "(%s)" tm))))
   " "))

(defun musicbrainz--format-series (s)
  (string-join
   (delq nil
         (list (plist-get s :name)
               (when-let* ((ty (plist-get s :type))) (format "[%s]" ty))))
   " "))

(defun musicbrainz--format-instrument (i)
  (string-join
   (delq nil
         (list (plist-get i :name)
               (when-let* ((ty (plist-get i :type))) (format "[%s]" ty))))
   " "))

(defun musicbrainz--format-collection (c)
  (or (plist-get c :name) ""))

(defun musicbrainz--format-url (u)
  (or (plist-get u :resource) (plist-get u :id) ""))

(defun musicbrainz--format-annotation (a)
  (or (plist-get a :name) ""))

(defun musicbrainz--format-tag (tag)
  (or (plist-get tag :name) ""))

(defun musicbrainz--format-cdstub (c)
  (format "%s%s"
          (or (plist-get c :title) "")
          (if-let* ((ar (plist-get c :artist))) (format " — %s" ar) "")))


;;; Search results buffer (tabulated-list-mode, elpaca-manager style)

(defvar-local musicbrainz--entity nil "Entity type string for this results buffer.")
(defvar-local musicbrainz--query nil "Query string (search mode) for this buffer.")
(defvar-local musicbrainz--linked nil "Linked entity type (browse mode).")
(defvar-local musicbrainz--linked-id nil "Linked entity MBID (browse mode).")
(defvar-local musicbrainz--mode nil "Either `search' or `browse'.")
(defvar-local musicbrainz--limit nil)
(defvar-local musicbrainz--offset nil)
(defvar-local musicbrainz--count nil)
(defvar-local musicbrainz--entries nil "Accumulated tabulated-list entries.")

(defvar musicbrainz-search-mode-map
  (let ((m (make-sparse-keymap)))
    (set-keymap-parent m tabulated-list-mode-map)
    (define-key m (kbd "RET") #'musicbrainz-show-at-point)
    (define-key m (kbd "+") #'musicbrainz-search-more)
    m)
  "Keymap for `musicbrainz-search-mode'.")

(define-derived-mode musicbrainz-search-mode tabulated-list-mode "MB-Search"
  "Major mode for MusicBrainz search results."
  :group 'musicbrainz
  (setq tabulated-list-format [("Summary" 70 nil)
                               ("Info" 22 nil)
                               ("MBID" 36 nil)])
  (tabulated-list-init-header))

(defun musicbrainz--entry-info (entity item)
  "Secondary column text for ITEM of ENTITY."
  (pcase entity
    ("artist" (or (plist-get item :country) ""))
    ("release" (string-join (delq nil (list (plist-get item :date)
                                            (plist-get item :status)))
                            " "))
    ("recording" (musicbrainz--ms (plist-get item :length)))
    ("label" (or (plist-get item :label-code) ""))
    ("release-group" (or (plist-get item :first-release-date) ""))
    (_ "")))

(defun musicbrainz--entry-summary (entity item)
  (pcase entity
    ("artist" (musicbrainz--format-artist item))
    ("release" (musicbrainz--format-release item))
    ("recording" (musicbrainz--format-recording item))
    ("label" (musicbrainz--format-label item))
    ("release-group" (musicbrainz--format-release-group item))
    ("work" (musicbrainz--format-work item))
    ("area" (musicbrainz--format-area item))
    ("place" (musicbrainz--format-place item))
    ("event" (musicbrainz--format-event item))
    ("series" (musicbrainz--format-series item))
    ("instrument" (musicbrainz--format-instrument item))
    ("collection" (musicbrainz--format-collection item))
    ("url" (musicbrainz--format-url item))
    ("annotation" (musicbrainz--format-annotation item))
    ("tag" (musicbrainz--format-tag item))
    ("cdstub" (musicbrainz--format-cdstub item))
    (_ (or (plist-get item :title) (plist-get item :name) ""))))

(defun musicbrainz--list-key (entity)
  "Search-result list key for ENTITY (mirrors TS I*List shapes)."
  (plist-get (cdr (assoc entity musicbrainz--entities)) :list))

(defun musicbrainz--make-entries (entity items)
  (mapcar (lambda (it)
            (list (or (plist-get it :id) "")
                  (vector (musicbrainz--entry-summary entity it)
                          (musicbrainz--entry-info entity it)
                          (or (plist-get it :id) ""))))
          items))

(defun musicbrainz--refresh-header ()
  (setq header-line-format
        (format " %s %s — %d of %s (RET detail, + more, g refresh, q quit)"
                (musicbrainz--entity-label musicbrainz--entity)
                (if (eq musicbrainz--mode 'browse)
                    (format "by %s %s" musicbrainz--linked musicbrainz--linked-id)
                  (format "\"%s\"" musicbrainz--query))
                (length musicbrainz--entries)
                (or musicbrainz--count "?"))))

(defun musicbrainz--run-search (entity query limit offset)
  (musicbrainz--call (concat "search-" entity)
                   (list :query query :limit limit :offset offset)))

(defun musicbrainz--run-browse (entity linked linked-id limit offset)
  (musicbrainz--call (concat "browse-" entity)
                   (list (intern (concat ":" linked)) linked-id
                         :limit limit :offset offset)))

(defun musicbrainz--run-page (limit offset)
  "Fetch one page for the current buffer (search or browse mode)."
  (if (eq musicbrainz--mode 'browse)
      (musicbrainz--run-browse musicbrainz--entity musicbrainz--linked musicbrainz--linked-id limit offset)
    (musicbrainz--run-search musicbrainz--entity musicbrainz--query limit offset)))

(defun musicbrainz--show-results-buffer (buf)
  ;; NOTE: callers must enable `musicbrainz-search-mode' BEFORE setting
  ;; the mb-- buffer-locals: entering a major mode kills all locals.
  ;; The buffer is shown before fetching so slow/failing requests still
  ;; leave visible UI (errors render in-buffer instead of nowhere).
  (with-current-buffer buf
    (musicbrainz--refresh-header)
    (condition-case err
        (let* ((res (musicbrainz--run-page musicbrainz--limit musicbrainz--offset))
               (items (seq-into (plist-get res (musicbrainz--list-key musicbrainz--entity)) 'list)))
          (setq musicbrainz--count (plist-get res :count)
                musicbrainz--entries (musicbrainz--make-entries musicbrainz--entity items)
                tabulated-list-entries musicbrainz--entries)
          (tabulated-list-print t)
          (musicbrainz--refresh-header))
      (error
       (setq tabulated-list-entries nil)
       (tabulated-list-print t)
       (setq header-line-format
             (format " Error: %s (q to quit)" (error-message-string err)))
       (message "MusicBrainz request failed: %s" (error-message-string err)))))
  (pop-to-buffer buf))

(defun musicbrainz-search (entity query)
  "Search MusicBrainz ENTITY for QUERY, showing a results buffer."
  (interactive
   (list (completing-read "Entity: " (musicbrainz--entities-where :search)
                           nil t nil nil "artist")
         (read-string "Query (e.g. artist:radiohead): ")))
  (when (string-empty-p query)
    (user-error "Empty query"))
  (let ((buf (get-buffer-create (format "*musicbrainz:%s:%s*" entity query))))
    (with-current-buffer buf
      (musicbrainz-search-mode)
      (setq musicbrainz--entity entity
            musicbrainz--query query
            musicbrainz--mode 'search
            musicbrainz--limit musicbrainz-limit
            musicbrainz--offset 0
            musicbrainz--entries nil)
      (message "Searching %s for %S..." entity query)
      (pop-to-buffer buf))
    (musicbrainz--show-results-buffer buf)))

(defun musicbrainz-browse (entity linked linked-id)
  "Browse ENTITY linked to LINKED entity MBID LINKED-ID."
  (interactive
   (let* ((en (completing-read "Browse entity: " (musicbrainz--entities-where :browse)
                               nil t nil nil "release"))
          (lk (completing-read "Linked by: "
                               (musicbrainz--entity-prop en :links) nil t))
          (id (read-string (format "%s MBID%s: " lk (if (equal lk "resource") " or URI" "")))))
     (list en lk id)))
  (when (string-empty-p linked-id)
    (user-error "Empty MBID"))
  (let ((buf (get-buffer-create (format "*musicbrainz:browse-%s:%s*" entity linked-id))))
    (with-current-buffer buf
      (musicbrainz-search-mode)
      (setq musicbrainz--entity entity
            musicbrainz--linked linked
            musicbrainz--linked-id linked-id
            musicbrainz--mode 'browse
            musicbrainz--limit musicbrainz-limit
            musicbrainz--offset 0
            musicbrainz--entries nil)
      (message "Browsing %s by %s %s..." entity linked linked-id)
      (pop-to-buffer buf))
    (musicbrainz--show-results-buffer buf)))

(defun musicbrainz-search-more ()
  "Load the next page of results into the current results buffer."
  (interactive nil musicbrainz-search-mode)
  (let ((next (+ musicbrainz--offset musicbrainz--limit)))
    (when (and musicbrainz--count (>= next musicbrainz--count))
      (user-error "No more results"))
    (message "Loading more...")
    (let* ((res (musicbrainz--run-page musicbrainz--limit next))
           (items (seq-into (plist-get res (musicbrainz--list-key musicbrainz--entity)) 'list)))
      (setq musicbrainz--offset next
            musicbrainz--entries (append musicbrainz--entries
                                (musicbrainz--make-entries musicbrainz--entity items))
            tabulated-list-entries musicbrainz--entries)
      (tabulated-list-print t)
      (musicbrainz--refresh-header))))

(defun musicbrainz-show-at-point ()
  "Open a detail buffer for the result on the current line."
  (interactive nil musicbrainz-search-mode)
  (let ((id (tabulated-list-get-id)))
    (unless (and id (not (string-empty-p id)))
      (user-error "No MBID on this line"))
    (musicbrainz-lookup musicbrainz--entity id)))


;;; Detail buffers (special-mode, BrainzWrap section layout)

(defvar musicbrainz-detail-mode-map
  (let ((m (make-sparse-keymap)))
    (set-keymap-parent m special-mode-map)
    m)
  "Keymap for `musicbrainz-detail-mode'.")

(define-derived-mode musicbrainz-detail-mode special-mode "MB-Detail"
  "Major mode for a single MusicBrainz entity."
  :group 'musicbrainz)

(defun musicbrainz--meta (label value)
  (when (and value (not (equal value "")))
    (insert (propertize (format "%-14s " label) 'face 'bold)
            (format "%s\n" value))))

(defun musicbrainz--aliases-section (entity)
  "Render ENTITY's aliases list."
  (when-let* ((aliases (plist-get entity :aliases)))
    (let ((items (seq-into aliases 'list)))
      (when items
        (insert (format "\nAliases (%d)\n" (seq-length items)))
        (dolist (a items)
          (insert (format "- %s%s%s\n"
                          (or (plist-get a :name) "")
                          (if-let* ((lc (plist-get a :locale)))
                              (format " [%s]" lc)
                            "")
                          (if-let* ((ty (plist-get a :type)))
                              (format " (%s)" ty)
                            ""))))))))

(defun musicbrainz--rating-string (entity)
  "Render ENTITY's rating as \"value (votes)\" or nil."
  (when-let* ((r (plist-get entity :rating)))
    (format "%s (%s votes)"
            (or (plist-get r :value) "?")
            (or (plist-get r :votes-count) "?"))))

(defun musicbrainz--tags-section (entity)
  "Render ENTITY's tags list, mirroring BrainzWrap's tags section."
  (when-let* ((tags (plist-get entity :tags)))
    (let ((items (seq-into tags 'list)))
      (when items
        (insert (format "\nTags (%d)\n" (seq-length items)))
        (dolist (tag items)
          (insert (format "- %s%s\n"
                          (or (plist-get tag :name) "")
                          (if-let* ((c (plist-get tag :count)))
                              (format " (%s)" c)
                            ""))))))))

(defun musicbrainz--genres-section (entity)
  "Render ENTITY's genres list; each name jumps to a recording search."
  (when-let* ((genres (plist-get entity :genres)))
    (let ((items (seq-into genres 'list)))
      (when items
        (insert (format "\nGenres (%d)\n" (seq-length items)))
        (dolist (g items)
          (let ((name (or (plist-get g :name) "")))
            (insert "- ")
            (unless (string-empty-p name)
              (insert-text-button name
                                  'action (lambda (_) (musicbrainz-search "recording" (format "genre:\"%s\"" name)))
                                  'follow-link t
                                  'help-echo (format "Search recordings tagged %s" name)))
            (insert (format "%s\n"
                            (if-let* ((c (plist-get g :count)))
                                (format " (%s)" c)
                              "")))))))))

(defun musicbrainz--sameas-section (entity)
  "Render ENTITY's sameAs links as labeled buttons, not bare URLs."
  (when-let* ((links (plist-get entity :sameAs)))
    (let ((items (seq-into links 'list)))
      (when items
        (insert (format "\nLinks (%d)\n" (seq-length items)))
        (dolist (l items)
          (let ((url (plist-get l :url))
                (label (format "[%s]" (or (plist-get l :type) "link"))))
            (insert "- ")
            (if (and url (not (string-empty-p url)))
                (insert-text-button label
                                    'action (lambda (_) (browse-url url))
                                    'follow-link t
                                    'help-echo url)
              (insert label))
            (insert "\n")))))))

(defun musicbrainz--mbid-button (label entity id)
  "Insert LABEL text; RET on it looks up ENTITY/ID."
  (insert-text-button (or label id)
                      'action (lambda (_) (musicbrainz-lookup entity id))
                      'follow-link t
                      'help-echo (format "%s %s" entity id))
  (insert "\n"))

(defun musicbrainz--detail-artist (a)
  (musicbrainz--meta "Type" (plist-get a :type))
  (musicbrainz--meta "Country" (plist-get a :country))
  (musicbrainz--meta "Sort Name" (plist-get a :sort-name))
  (musicbrainz--meta "Disambiguation" (plist-get a :disambiguation))
  (musicbrainz--meta "Rating" (musicbrainz--rating-string a))
  (when-let* ((ls (plist-get a :life-span)))
    (insert "\nLife Span\n")
    (musicbrainz--meta "Begin" (plist-get ls :begin))
    (musicbrainz--meta "End" (plist-get ls :end)))
  (musicbrainz--aliases-section a)
  (musicbrainz--tags-section a)
  (musicbrainz--genres-section a)
  (musicbrainz--sameas-section a))

(defun musicbrainz--detail-release (r)
  (musicbrainz--meta "Status" (plist-get r :status))
  (musicbrainz--meta "Date" (plist-get r :date))
  (musicbrainz--meta "Country" (plist-get r :country))
  (musicbrainz--meta "Barcode" (plist-get r :barcode))
  (musicbrainz--meta "ASIN" (plist-get r :asin))
  (musicbrainz--meta "Quality" (plist-get r :quality))
  (musicbrainz--meta "Packaging" (plist-get r :packaging))
  (when-let* ((tr (plist-get r :text-representation)))
    (musicbrainz--meta "Text" (format "%s/%s" (plist-get tr :language) (plist-get tr :script))))
  (when-let* ((ca (plist-get r :cover-art-archive)))
    (musicbrainz--meta "CoverArt" (format "%s front=%s back=%s"
                                 (plist-get ca :count)
                                 (plist-get ca :front) (plist-get ca :back))))
  (when-let* ((evs (plist-get r :release-events)))
    (insert (format "\nEvents (%d)\n" (seq-length evs)))
    (seq-doseq (ev (seq-into evs 'list))
      (insert (format "- %s%s\n" (or (plist-get ev :date) "")
                      (if-let* ((ar (plist-get ev :area)))
                          (format " (%s)" (plist-get ar :name))
                        "")))))
  (musicbrainz--meta "Artists" (musicbrainz--credit-string r))
  (when-let* ((rg (plist-get r :release-group)))
    (musicbrainz--meta "Group" (format "%s [%s]"
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
  (musicbrainz--aliases-section r)
  (musicbrainz--tags-section r)
  (musicbrainz--genres-section r)
  (musicbrainz--sameas-section r)
  (when-let* ((media (plist-get r :media)))
    (seq-doseq (m (seq-into media 'list))
      (insert (format "\n[%s]\n" (or (plist-get m :format) "Medium")))
      (seq-doseq (tr (seq-into (plist-get m :tracks) 'list))
        (insert (format "  %2s. %-40s %s  "
                        (or (plist-get tr :number) "")
                        (or (plist-get tr :title) "")
                        (musicbrainz--ms (plist-get tr :length))))
        (when-let* ((rid (plist-get (plist-get tr :recording) :id)))
          (musicbrainz--mbid-button rid "recording" rid))))))

(defun musicbrainz--detail-recording (r)
  (musicbrainz--meta "Length" (musicbrainz--ms (plist-get r :length)))
  (musicbrainz--meta "Video" (if (musicbrainz--false (plist-get r :video)) "yes" "no"))
  (musicbrainz--meta "Rating" (musicbrainz--rating-string r))
  (musicbrainz--meta "Artists" (musicbrainz--credit-string r))
  (when-let* ((isrcs (plist-get r :isrcs)))
    (musicbrainz--meta "ISRCs" (string-join (seq-into isrcs 'list) ", ")))
  (musicbrainz--aliases-section r)
  (musicbrainz--tags-section r)
  (musicbrainz--genres-section r)
  (musicbrainz--sameas-section r)
  (when-let* ((rels (plist-get r :releases)))
    (insert (format "\nReleases (%d)\n" (seq-length rels)))
    (seq-doseq (rel (seq-into rels 'list))
      (musicbrainz--mbid-button (plist-get rel :title) "release"
                       (plist-get rel :id)))))

(defun musicbrainz--detail-disc (d)
  (musicbrainz--meta "Sectors" (number-to-string (or (plist-get d :sectors) 0)))
  (when-let* ((rels (plist-get d :releases)))
    (insert (format "\nReleases (%d)\n" (seq-length rels)))
    (seq-doseq (rel (seq-into rels 'list))
      (musicbrainz--mbid-button (plist-get rel :title) "release"
                       (plist-get rel :id)))))

(defun musicbrainz--detail-label (l)
  (musicbrainz--meta "Type" (plist-get l :type))
  (musicbrainz--meta "Country" (plist-get l :country))
  (musicbrainz--meta "Sort Name" (plist-get l :sort-name))
  (musicbrainz--meta "Label Code" (plist-get l :label-code))
  (musicbrainz--meta "Disambiguation" (plist-get l :disambiguation))
  (musicbrainz--meta "Rating" (musicbrainz--rating-string l))
  (when-let* ((ar (plist-get l :area)))
    (musicbrainz--meta "Area" (plist-get ar :name)))
  (when-let* ((ipis (plist-get l :ipis)))
    (musicbrainz--meta "IPIs" (string-join (seq-into ipis 'list) ", ")))
  (when-let* ((isnis (plist-get l :isnis)))
    (musicbrainz--meta "ISNIs" (string-join (seq-into isnis 'list) ", ")))
  (when-let* ((ls (plist-get l :life-span)))
    (insert "\nLife Span\n")
    (musicbrainz--meta "Begin" (plist-get ls :begin))
    (musicbrainz--meta "End" (plist-get ls :end)))
  (musicbrainz--aliases-section l)
  (musicbrainz--tags-section l)
  (musicbrainz--genres-section l)
  (musicbrainz--sameas-section l))

(defun musicbrainz--detail-release-group (g)
  (musicbrainz--meta "Type" (plist-get g :type))
  (musicbrainz--meta "Disambiguation" (plist-get g :disambiguation))
  (musicbrainz--meta "First Date" (plist-get g :first-release-date))
  (musicbrainz--meta "Primary" (plist-get g :primary-type))
  (when-let* ((st (plist-get g :secondary-types)))
    (musicbrainz--meta "Secondary" (string-join (seq-into st 'list) ", ")))
  (musicbrainz--meta "Artists" (musicbrainz--credit-string g))
  (musicbrainz--meta "Rating" (musicbrainz--rating-string g))
  (when-let* ((rels (plist-get g :releases)))
    (insert (format "\nReleases (%d)\n" (seq-length rels)))
    (seq-doseq (rel (seq-into rels 'list))
      (musicbrainz--mbid-button (plist-get rel :title) "release"
                       (plist-get rel :id))))
  (musicbrainz--aliases-section g)
  (musicbrainz--tags-section g)
  (musicbrainz--genres-section g)
  (musicbrainz--sameas-section g))

(defun musicbrainz--detail-work (w)
  (musicbrainz--meta "Type" (plist-get w :type))
  (musicbrainz--meta "Disambiguation" (plist-get w :disambiguation))
  (musicbrainz--meta "Language" (plist-get w :language))
  (when-let* ((langs (plist-get w :languages)))
    (musicbrainz--meta "Languages" (string-join (seq-into langs 'list) ", ")))
  (when-let* ((iswcs (plist-get w :iswcs)))
    (musicbrainz--meta "ISWCs" (string-join (seq-into iswcs 'list) ", ")))
  (musicbrainz--meta "Rating" (musicbrainz--rating-string w))
  (musicbrainz--aliases-section w)
  (when-let* ((attrs (plist-get w :attributes)))
    (insert (format "\nAttributes (%d)\n" (seq-length attrs)))
    (seq-doseq (a (seq-into attrs 'list))
      (insert (format "- %s: %s\n"
                      (or (plist-get a :type) "")
                      (or (plist-get a :value) "")))))
  (musicbrainz--tags-section w)
  (musicbrainz--genres-section w)
  (musicbrainz--sameas-section w))

(defun musicbrainz--detail-generic (e)
  "Fallback renderer: print scalar fields, then shared sections.
Covers area/place/event/series/instrument/collection/url and any
future entity without a dedicated renderer."
  (musicbrainz--meta "Rating" (musicbrainz--rating-string e))
  (dolist (kv '((:name . "Name") (:title . "Title") (:type . "Type")
                (:disambiguation . "Disambiguation")
                (:sort-name . "Sort Name") (:country . "Country")
                (:address . "Address") (:description . "Description")
                (:language . "Language") (:editor . "Editor")
                (:entity-type . "Entity Type") (:time . "Time")
                (:setlist . "Setlist")
                (:cancelled . "Cancelled") (:resource . "Resource")
                (:barcode . "Barcode") (:comment . "Comment")
                (:artist . "Artist") (:label-code . "Label Code")))
    (let ((v (plist-get e (car kv))))
      (when (and v (not (equal v "")) (atom v))
        (musicbrainz--meta (cdr kv) (format "%s" v)))))
  (musicbrainz--aliases-section e)
  (when-let* ((ls (plist-get e :life-span)))
    (insert "\nLife Span\n")
    (musicbrainz--meta "Begin" (plist-get ls :begin))
    (musicbrainz--meta "End" (plist-get ls :end)))
  (when-let* ((iso (plist-get e :iso-3166-1-codes)))
    (musicbrainz--meta "ISO" (string-join (seq-into iso 'list) ", ")))
  (when-let* ((co (plist-get e :coordinates)))
    (musicbrainz--meta "Coords" (format "%s, %s" (plist-get co :latitude)
                               (plist-get co :longitude))))
  (musicbrainz--tags-section e)
  (musicbrainz--genres-section e)
  (musicbrainz--sameas-section e))

(defun musicbrainz-lookup (entity mbid)
  "Show a detail buffer for ENTITY MBID."
  (interactive
   (list (completing-read "Entity: " (musicbrainz--entities-where :lookup)
                           nil t nil nil "artist")
         (read-string "MBID: ")))
  (when (string-empty-p mbid)
    (user-error "Empty MBID"))
  (message "Looking up %s %s..." entity mbid)
  (let ((buf (get-buffer-create (format "*musicbrainz:%s:%s*" entity mbid))))
    (with-current-buffer buf
      (musicbrainz-detail-mode)
      (let ((inhibit-read-only t))
        (erase-buffer)
        (insert (propertize (format "%s %s\n\n" (musicbrainz--entity-label entity) mbid)
                            'face 'bold))
        (insert "Loading...\n"))
      (pop-to-buffer buf))
    (with-current-buffer buf
      (condition-case err
          (let ((res (musicbrainz--call (concat "lookup-" entity)
                                        (list :id mbid))))
            (let ((inhibit-read-only t))
              (erase-buffer)
              (insert (propertize (format "%s %s\n\n" (musicbrainz--entity-label entity) mbid)
                                  'face 'bold))
              (pcase entity
                ("artist" (musicbrainz--detail-artist res))
                ("release" (musicbrainz--detail-release res))
                ("recording" (musicbrainz--detail-recording res))
                ("discid" (musicbrainz--detail-disc res))
                ("label" (musicbrainz--detail-label res))
                ("release-group" (musicbrainz--detail-release-group res))
                ("work" (musicbrainz--detail-work res))
                (_ (musicbrainz--detail-generic res)))
              (goto-char (point-min))))
        (error
         (let ((inhibit-read-only t))
           (erase-buffer)
           (insert (format "Error: %s\n" (error-message-string err))))
         (message "MusicBrainz lookup failed: %s" (error-message-string err)))))))

(provide 'musicbrainz)
;;; musicbrainz.el ends here
