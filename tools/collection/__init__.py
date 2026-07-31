# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Offline integrity + validation tools for the data-collection overlay.

These scripts run against the on-disk artefacts a collection-mode session
produces (SESSION_HEADER.json, reference/chunks.jsonl + WAV chunks,
events/{accepted,rejected}/*.wav, decisions.jsonl, SESSION_END.json).

See DATA_COLLECTION_IMPL_VALIDATION_PLAN.md §3 for the checks and their
pre-registered falsifiers.
"""
