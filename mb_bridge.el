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

(defvar mb--entities
  ;; :links mirrors the C++ browse_links (BrowseXEntityParams in TS).
  '(("artist"       :search t :lookup t :browse t :list artists
      :links ("area" "collection" "recording" "release" "release-group" "work"))
    ("release"      :search t :lookup t :browse t :list releases
      :links ("area" "artist" "editor" "event" "label" "place" "recording" "release" "release-group" "track_artist" "work"))
    ("recording"    :search t :lookup t :browse t :list recordings
      :links ("artist" "collection" "release" "work"))
    ("label"        :search t :lookup t :browse t :list labels
      :links ("area" "collection" "release"))
    ("release-group" :search t :lookup t :browse t :list release-groups
      :links ("artist" "collection" "release"))
    ("work"         :search t :lookup t :browse t :list works
      :links ("artist" "collection"))
    ("area"         :search t :lookup t :browse t :list areas
      :links ("collection"))
    ("place"        :search t :lookup t :browse t :list places
      :links ("area" "collection"))
    ("event"        :search t :lookup t :browse t :list events
      :links ("area" "artist" "collection" "place"))
    ("series"       :search t :lookup t :browse t :list series
      :links ("collection"))
    ("instrument"  :search t :lookup t :browse t :list instruments
      :links ("collection"))
    ("collection"   :search nil :lookup t :browse t :list collections
      :links ("area" "artist" "editor" "event" "label" "place" "recording" "release" "release-group" "work"))
    ("url"          :search t :lookup t :browse t :list urls
      :links ("resource"))
    ("annotation"   :search t :lookup nil :browse nil :list annotations)
    ("tag"          :search t :lookup nil :browse nil :list tags)
    ("cdstub"       :search t :lookup nil :browse nil :list cdstubs)
    ("discid"       :search nil :lookup t :browse nil :list releases))
  "Entity capability table. Keys mirror musicbrainz-api's method set:
searchable/lookable/browsable per entity; :list is the search key;
:links mirrors the C++ browse_links (TS BrowseXEntityParams).")

(defun mb--entities-where (prop)
  (mapcar #'car (seq-filter (lambda (e) (plist-get (cdr e) prop))
                            mb--entities)))

(defun mb--entity-prop (entity prop)
  (plist-get (cdr (assoc entity mb--entities)) prop))

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

(defun mb--format-label (l)
  (string-join
   (delq nil
         (list (plist-get l :name)
               (when-let* ((ty (plist-get l :type))) (format "[%s]" ty))
               (when-let* ((code (plist-get l :label-code))) (format "(LC %s)" code))))
   " "))

(defun mb--format-release-group (g)
  (format "%s%s%s"
          (or (plist-get g :title) "")
          (if-let* ((d (plist-get g :first-release-date))) (format " (%s)" d) "")
          (if-let* ((p (plist-get g :primary-type))) (format " [%s]" p) "")))

(defun mb--format-work (w)
  (string-join
   (delq nil
         (list (plist-get w :title)
               (when-let* ((ty (plist-get w :type))) (format "[%s]" ty))
               (when-let* ((lang (plist-get w :language))) (format "(%s)" lang))))
   " "))

(defun mb--format-area (a)
  (string-join
   (delq nil
         (list (plist-get a :name)
               (when-let* ((ty (plist-get a :type))) (format "[%s]" ty))))
   " "))

(defun mb--format-place (p)
  (string-join
   (delq nil
         (list (plist-get p :name)
               (when-let* ((ty (plist-get p :type))) (format "[%s]" ty))
               (when-let* ((ad (plist-get p :address))) (format "(%s)" ad))))
   " "))

(defun mb--format-event (e)
  (string-join
   (delq nil
         (list (plist-get e :name)
               (when-let* ((ty (plist-get e :type))) (format "[%s]" ty))
               (when-let* ((tm (plist-get e :time))) (format "(%s)" tm))))
   " "))

(defun mb--format-series (s)
  (string-join
   (delq nil
         (list (plist-get s :name)
               (when-let* ((ty (plist-get s :type))) (format "[%s]" ty))))
   " "))

(defun mb--format-instrument (i)
  (string-join
   (delq nil
         (list (plist-get i :name)
               (when-let* ((ty (plist-get i :type))) (format "[%s]" ty))))
   " "))

(defun mb--format-collection (c)
  (or (plist-get c :name) ""))

(defun mb--format-url (u)
  (or (plist-get u :resource) (plist-get u :id) ""))

(defun mb--format-annotation (a)
  (or (plist-get a :name) ""))

(defun mb--format-tag (tag)
  (or (plist-get tag :name) ""))

(defun mb--format-cdstub (c)
  (format "%s%s"
          (or (plist-get c :title) "")
          (if-let* ((ar (plist-get c :artist))) (format " — %s" ar) "")))


;;; Search results buffer (tabulated-list-mode, elpaca-manager style)

(defvar-local mb--entity nil "Entity type string for this results buffer.")
(defvar-local mb--query nil "Query string (search mode) for this buffer.")
(defvar-local mb--linked nil "Linked entity type (browse mode).")
(defvar-local mb--linked-id nil "Linked entity MBID (browse mode).")
(defvar-local mb--mode nil "Either `search' or `browse'.")
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
    ("label" (or (plist-get item :label-code) ""))
    ("release-group" (or (plist-get item :first-release-date) ""))
    (_ "")))

(defun mb--entry-summary (entity item)
  (pcase entity
    ("artist" (mb--format-artist item))
    ("release" (mb--format-release item))
    ("recording" (mb--format-recording item))
    ("label" (mb--format-label item))
    ("release-group" (mb--format-release-group item))
    ("work" (mb--format-work item))
    ("area" (mb--format-area item))
    ("place" (mb--format-place item))
    ("event" (mb--format-event item))
    ("series" (mb--format-series item))
    ("instrument" (mb--format-instrument item))
    ("collection" (mb--format-collection item))
    ("url" (mb--format-url item))
    ("annotation" (mb--format-annotation item))
    ("tag" (mb--format-tag item))
    ("cdstub" (mb--format-cdstub item))
    (_ (or (plist-get item :title) (plist-get item :name) ""))))

(defun mb--list-key (entity)
  "Search-result list key for ENTITY (mirrors TS I*List shapes)."
  (plist-get (cdr (assoc entity mb--entities)) :list))

(defun mb--make-entries (entity items)
  (mapcar (lambda (it)
            (list (or (plist-get it :id) "")
                  (vector (mb--entry-summary entity it)
                          (mb--entry-info entity it)
                          (or (plist-get it :id) ""))))
          items))

(defun mb--refresh-header ()
  (setq header-line-format
        (format " %s %s — %d of %s (RET detail, + more, g refresh, q quit)"
                (mb--entity-label mb--entity)
                (if (eq mb--mode 'browse)
                    (format "by %s %s" mb--linked mb--linked-id)
                  (format "\"%s\"" mb--query))
                (length mb--entries)
                (or mb--count "?"))))

(defun mb--run-search (entity query limit offset)
  (mb-bridge--call (concat "search-" entity)
                   (list :query query :limit limit :offset offset)))

(defun mb--run-browse (entity linked linked-id limit offset)
  (mb-bridge--call (concat "browse-" entity)
                   (list (intern (concat ":" linked)) linked-id
                         :limit limit :offset offset)))

(defun mb--run-page (limit offset)
  "Fetch one page for the current buffer (search or browse mode)."
  (if (eq mb--mode 'browse)
      (mb--run-browse mb--entity mb--linked mb--linked-id limit offset)
    (mb--run-search mb--entity mb--query limit offset)))

(defun mb--show-results-buffer (buf)
  (with-current-buffer buf
    (mb-search-mode)
    (let* ((res (mb--run-page mb--limit mb--offset))
           (items (seq-into (plist-get res (mb--list-key mb--entity)) 'list)))
      (setq mb--count (plist-get res :count)
            mb--entries (mb--make-entries mb--entity items)
            tabulated-list-entries mb--entries)
      (tabulated-list-print t)
      (mb--refresh-header)))
  (pop-to-buffer buf))

(defun mb-search (entity query)
  "Search MusicBrainz ENTITY for QUERY, showing a results buffer."
  (interactive
   (list (completing-read "Entity: " (mb--entities-where :search)
                           nil t nil nil "artist")
         (read-string "Query (e.g. artist:radiohead): ")))
  (let ((buf (get-buffer-create (format "*mb:%s:%s*" entity query))))
    (with-current-buffer buf
      (setq mb--entity entity
            mb--query query
            mb--mode 'search
            mb--limit mb-bridge-limit
            mb--offset 0
            mb--entries nil)
      (message "Searching %s for %S..." entity query))
    (mb--show-results-buffer buf)))

(defun mb-browse (entity linked linked-id)
  "Browse ENTITY linked to LINKED entity MBID LINKED-ID."
  (interactive
   (let* ((en (completing-read "Browse entity: " (mb--entities-where :browse)
                               nil t nil nil "release"))
          (lk (completing-read "Linked by: "
                               (mb--entity-prop en :links) nil t))
          (id (read-string (format "%s MBID%s: " lk (if (equal lk "resource") " or URI" "")))))
     (list en lk id)))
  (let ((buf (get-buffer-create (format "*mb:browse-%s:%s*" entity linked-id))))
    (with-current-buffer buf
      (setq mb--entity entity
            mb--linked linked
            mb--linked-id linked-id
            mb--mode 'browse
            mb--limit mb-bridge-limit
            mb--offset 0
            mb--entries nil)
      (message "Browsing %s by %s %s..." entity linked linked-id))
    (mb--show-results-buffer buf)))

(defun mb-search-more ()
  "Load the next page of results into the current results buffer."
  (interactive nil mb-search-mode)
  (let ((next (+ mb--offset mb--limit)))
    (when (and mb--count (>= next mb--count))
      (user-error "No more results"))
    (message "Loading more...")
    (let* ((res (mb--run-page mb--limit next))
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

(defun mb--tags-section (entity)
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

(defun mb--genres-section (entity)
  "Render ENTITY's genres list."
  (when-let* ((genres (plist-get entity :genres)))
    (let ((items (seq-into genres 'list)))
      (when items
        (insert (format "\nGenres (%d)\n" (seq-length items)))
        (dolist (g items)
          (insert (format "- %s%s\n"
                          (or (plist-get g :name) "")
                          (if-let* ((c (plist-get g :count)))
                              (format " (%s)" c)
                            ""))))))))

(defun mb--sameas-section (entity)
  "Render ENTITY's sameAs external links as clickable buttons."
  (when-let* ((links (plist-get entity :sameAs)))
    (let ((items (seq-into links 'list)))
      (when items
        (insert (format "\nLinks (%d)\n" (seq-length items)))
        (dolist (l items)
          (let ((url (plist-get l :url)))
            (insert (format "- %s "
                            (or (plist-get l :type) "link")))
            (when (and url (not (string-empty-p url)))
              (insert-text-button url
                                  'action (lambda (_) (browse-url url))
                                  'follow-link t
                                  'help-echo url))
            (insert "\n")))))))

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
    (mb--meta "End" (plist-get ls :end)))
  (mb--tags-section a)
  (mb--genres-section a)
  (mb--sameas-section a))

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
  (mb--tags-section r)
  (mb--genres-section r)
  (mb--sameas-section r)
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
  (mb--tags-section r)
  (mb--genres-section r)
  (mb--sameas-section r)
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

(defun mb--detail-label (l)
  (mb--meta "Type" (plist-get l :type))
  (mb--meta "Country" (plist-get l :country))
  (mb--meta "Sort Name" (plist-get l :sort-name))
  (mb--meta "Label Code" (plist-get l :label-code))
  (mb--meta "Disambiguation" (plist-get l :disambiguation))
  (when-let* ((ar (plist-get l :area)))
    (mb--meta "Area" (plist-get ar :name)))
  (when-let* ((ipis (plist-get l :ipis)))
    (mb--meta "IPIs" (string-join (seq-into ipis 'list) ", ")))
  (when-let* ((isnis (plist-get l :isnis)))
    (mb--meta "ISNIs" (string-join (seq-into isnis 'list) ", ")))
  (when-let* ((ls (plist-get l :life-span)))
    (insert "\nLife Span\n")
    (mb--meta "Begin" (plist-get ls :begin))
    (mb--meta "End" (plist-get ls :end)))
  (mb--tags-section l)
  (mb--genres-section l)
  (mb--sameas-section l))

(defun mb--detail-release-group (g)
  (mb--meta "Type" (plist-get g :type))
  (mb--meta "Disambiguation" (plist-get g :disambiguation))
  (mb--meta "First Date" (plist-get g :first-release-date))
  (mb--meta "Primary" (plist-get g :primary-type))
  (when-let* ((st (plist-get g :secondary-types)))
    (mb--meta "Secondary" (string-join (seq-into st 'list) ", ")))
  (mb--meta "Artists" (mb--credit-string g))
  (when-let* ((rels (plist-get g :releases)))
    (insert (format "\nReleases (%d)\n" (seq-length rels)))
    (seq-doseq (rel (seq-into rels 'list))
      (mb--mbid-button (plist-get rel :title) "release"
                       (plist-get rel :id))))
  (mb--tags-section g)
  (mb--genres-section g)
  (mb--sameas-section g))

(defun mb--detail-work (w)
  (mb--meta "Type" (plist-get w :type))
  (mb--meta "Disambiguation" (plist-get w :disambiguation))
  (mb--meta "Language" (plist-get w :language))
  (when-let* ((langs (plist-get w :languages)))
    (mb--meta "Languages" (string-join (seq-into langs 'list) ", ")))
  (when-let* ((iswcs (plist-get w :iswcs)))
    (mb--meta "ISWCs" (string-join (seq-into iswcs 'list) ", ")))
  (when-let* ((attrs (plist-get w :attributes)))
    (insert (format "\nAttributes (%d)\n" (seq-length attrs)))
    (seq-doseq (a (seq-into attrs 'list))
      (insert (format "- %s: %s\n"
                      (or (plist-get a :type) "")
                      (or (plist-get a :value) "")))))
  (mb--tags-section w)
  (mb--genres-section w)
  (mb--sameas-section w))

(defun mb--detail-generic (e)
  "Fallback renderer: print scalar fields, then shared sections.
Covers area/place/event/series/instrument/collection/url and any
future entity without a dedicated renderer."
  (dolist (kv '((:name . "Name") (:title . "Title") (:type . "Type")
                (:disambiguation . "Disambiguation")
                (:sort-name . "Sort Name") (:country . "Country")
                (:address . "Address") (:description . "Description")
                (:language . "Language")                 (:editor . "Editor")
                (:entity-type . "Entity Type") (:time . "Time")
                (:setlist . "Setlist")
                (:cancelled . "Cancelled") (:resource . "Resource")
                (:barcode . "Barcode") (:comment . "Comment")
                (:artist . "Artist") (:label-code . "Label Code")))
    (let ((v (plist-get e (car kv))))
      (when (and v (not (equal v "")) (atom v))
        (mb--meta (cdr kv) (format "%s" v)))))
  (when-let* ((ls (plist-get e :life-span)))
    (insert "\nLife Span\n")
    (mb--meta "Begin" (plist-get ls :begin))
    (mb--meta "End" (plist-get ls :end)))
  (when-let* ((iso (plist-get e :iso-3166-1-codes)))
    (mb--meta "ISO" (string-join (seq-into iso 'list) ", ")))
  (when-let* ((co (plist-get e :coordinates)))
    (mb--meta "Coords" (format "%s, %s" (plist-get co :latitude)
                               (plist-get co :longitude))))
  (mb--tags-section e)
  (mb--genres-section e)
  (mb--sameas-section e))

(defun mb-lookup (entity mbid)
  "Show a detail buffer for ENTITY MBID."
  (interactive
   (list (completing-read "Entity: " (mb--entities-where :lookup)
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
          ("discid" (mb--detail-disc res))
          ("label" (mb--detail-label res))
          ("release-group" (mb--detail-release-group res))
          ("work" (mb--detail-work res))
          (_ (mb--detail-generic res)))
        (mb-detail-mode)
        (goto-char (point-min))))
    (pop-to-buffer buf)))

(provide 'mb_bridge)
;;; mb_bridge.el ends here
