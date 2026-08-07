@echo off
rem Overnight loop. Double-click this, say what to work on, leave it running.
rem It builds, tests, screenshots, documents, commits and pushes on every iteration
rem without asking. Stop it with Ctrl+C, or by creating a file called STOP-LOOP next
rem to this one -- which works from anywhere, including over a network share.
rem
rem See documentation/18-overnight-loop.md.
title WorldShaper overnight loop
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\loop.ps1" %*
