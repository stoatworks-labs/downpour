#pragma once

#include <string>

/**
    Logging for a plugin that lives inside somebody else's process.

    A small member of the fleet's `diag` family. The rest of the repos get a
    rotating log, a crash report and a diagnostics bundle; an FFGL plugin gets
    only the log, for two reasons:

    - **No crash handler.** A plugin loaded into Resolume must not install a
      process-wide signal handler. It would intercept faults that are not ours
      and interfere with the host's own handling. A plugin has no business
      deciding what happens when Resolume dies.
    - **No bundle command.** There is no UI to hang one off -- a plugin is a
      list of sliders in someone else's inspector.

    What it covers is the three failures that actually happen, all of which look
    identical from the operator's side ("it does nothing") with no message
    anywhere:

    - **The shader would not compile**, so `InitGL` returns `FF_FAIL`. The GL
      vendor, renderer and version strings are logged next to the compile log,
      because a shader that builds on one machine and not on another is a driver
      answer, not a source answer.
    - **A font would not load or has no glyph for anything asked of it.** The
      picture is then blank or a grid of identical boxes, and which of the two
      it is depends on a fallback that is invisible from outside.
    - **A text file would not open, or decoded to nothing.** A path that was
      valid on the machine where the composition was saved is the ordinary way
      this happens.
*/
namespace downpour::diag
{

/// Open the log file and record the plugin build, once per process.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Full path of the log file, for the README to point at.
std::string logPath();

} // namespace downpour::diag
