// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_BROWSER_CONTEXT_KEYED_SERVICE_BROWSER_CONTEXT_KEYED_SERVICE_EXPORT_H_
#define COMPONENTS_BROWSER_CONTEXT_KEYED_SERVICE_BROWSER_CONTEXT_KEYED_SERVICE_EXPORT_H_

#if defined(COMPONENT_BUILD)
#if defined(WIN32)

#if defined(BROWSER_CONTEXT_KEYED_SERVICE_IMPLEMENTATION)
#define BROWSER_CONTEXT_KEYED_SERVICE_EXPORT __declspec(dllexport)
#else
#define BROWSER_CONTEXT_KEYED_SERVICE_EXPORT __declspec(dllimport)
#endif  // defined(BROWSER_CONTEXT_KEYED_SERVICE_IMPLEMENTATION)

#else  // defined(WIN32)
#if defined(BROWSER_CONTEXT_KEYED_SERVICE_IMPLEMENTATION)
#define BROWSER_CONTEXT_KEYED_SERVICE_EXPORT __attribute__((visibility("default")))
#else
#define BROWSER_CONTEXT_KEYED_SERVICE_EXPORT
#endif
#endif

#else  // defined(COMPONENT_BUILD)
#define BROWSER_CONTEXT_KEYED_SERVICE_EXPORT
#endif

#endif  // COMPONENTS_BROWSER_CONTEXT_KEYED_SERVICE_BROWSER_CONTEXT_KEYED_SERVICE_EXPORT_H_
