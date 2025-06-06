AHAT - Android Heap Analysis Tool

Usage:
  java -jar ahat.jar [OPTIONS] FILE
    Launch an http server for viewing the given Android heap dump FILE.

  OPTIONS:
    -p <port>
       Serve pages on the given port. Defaults to 7100.
    --proguard-map FILE
       Use the proguard map FILE to deobfuscate the heap dump.
    --baseline FILE
       Diff the heap dump against the given baseline heap dump FILE.
    --baseline-proguard-map FILE
       Use the proguard map FILE to deobfuscate the baseline heap dump.
    --retained [strong | soft | finalizer | weak | phantom | unreachable]
       The weakest reachability of instances to treat as retained.
       Defaults to soft

TODO:
 * Add a user guide.
 * Dim 'image' and 'zygote' heap sizes slightly? Why do we even show these?
 * Let user re-sort sites objects info by clicking column headers.
 * Let user re-sort "Objects" list.
 * Show site context and heap and class filter in "Objects" view?
 * Have a menu at the top of an object view with links to the sections?
 * Include ahat version and hprof file in the menu at the top of the page?
 * Heaped Table
   - Make sortable by clicking on headers.
 * For HeapTable with single heap shown, the heap name isn't centered?
 * Consistently document functions.
 * Show version number with --version.
 * Show somewhere where to send bugs.
 * Include a link to /objects in the overview and menu?
 * Turn on LOCAL_JAVACFLAGS := -Xlint:unchecked -Werror

 * [low priority] by site allocations won't line up if the stack has been
   truncated. Is there any way to manually line them up in that case?

Things to Test:
 * That we can open a hprof without an 'app' heap and show a tabulation of
   objects normally sorted by 'app' heap by default.
 * Visit /objects without parameters and verify it doesn't throw an exception.
 * Visit /objects with an invalid site, verify it doesn't throw an exception.
 * That we can view the list of all objects in a reasonably short amount of
   time.
 * That we don't show the 'extra' column in the DominatedList if we are
   showing all the instances.
 * Instance.getDexCacheLocation

Reported Issues:
 * Request to be able to sort tables by size.

Known Issues:
 * Line number decoding for allocations in proguarded classes.

Release History:
 1.8 January 02, 2025
   Show string values of byte[] instances.
   Fix accounting for cleaned native registrations
   Add option to download the contents of a byte[] as a file.
   Show retained size in allocations view.

 1.7.3 June 27, 2024
   Add support to display bitmaps included in heapdump. To use this
   functionality, collect the heapdump with `adb shell am dumpheap -b <fmt>`,
   <fmt> can be `png`, `jpg` or `webp`.

 1.7.2 March 2, 2022
   Fix ahat parsing to allow leading whitespaces for comments.
   Hide Value class constructor.

 1.7.1 September 30, 2020
   Fix issue parsing proguard maps with comments.

 1.7 August 8, 2019
   Annotate binder services, tokens, and proxies.
   Add option for viewing subclass instances of a class.
   Recognize java.lang.ref.Finalizer as a finalizer reference.
   Minor bug fixes and API improvements.

 1.6 July 24, 2018
   Distinguish between soft/weak/phantom/etc references.
   Annotate $classOverhead byte[] arrays with their class.
   Show progress of heap dump processing.
   Add --retained command line option to ahat.
   Support heap dumps generated with HotSpotDiagnosticMXBean.
   Updated public APIs for dominators computation, reachability and parser.
   AhatInstance no longer implements DominatorsComputation.Node
   Bug fixes.

 1.5 December 05, 2017
   Distinguish between weakly reachable and unreachable instances.
   Allow hex ids to be used for objects in query parameters.
   Restore old presentation of sample paths from gc roots.
   Fix bug in selection of sample paths from gc root.
   Fix bug in proguard deobfuscation of stack frames.
   Tighten up and document ahat public API.

 1.4 October 03, 2017
   Give better error messages on failure to launch ahat.
   Properly mark thread and non-default root objects as roots.
   Improve startup performance, in some cases significantly.
   Other miscellaneous bug fixes.

 1.3.1 August 22, 2017
   Don't include weak references in sample paths.

 1.3 July 25, 2017
   Improve diffing of static and instance fields.
   Improve startup performance by roughly 25%.

 1.2 May 26, 2017
   Show registered native sizes of objects.
   Simplify presentation of sample path from gc root.

 1.1 Feb 21, 2017
   Show java.lang.ref.Reference referents as "unreachable" instead of null.

 1.0 Dec 20, 2016
   Add support for diffing two heap dumps.
   Remove native allocations view.
   Remove outdated help page.
   Significant refactoring of ahat internals.

 0.8 Oct 18, 2016
   Show sample path from GC root with field names in place of dominator path.

 0.7 Aug 16, 2016
   Launch ahat server before processing the heap dump.
   Target Java 1.7.

 0.6 Jun 21, 2016
   Add support for proguard deobfuscation.

 0.5 Apr 19, 2016
   Update perflib to perflib-25.0.0 to improve processing performance.

 0.4 Feb 23, 2016
   Annotate char[] objects with their string values.
   Show registered native allocations for heap dumps that support it.

 0.3 Dec 15, 2015
   Fix page loading performance by showing a limited number of entries by default.
   Fix mismatch between overview and "roots" totals.
   Annotate root objects and show their types.
   Annotate references with their referents.

 0.2 Oct 20, 2015
   Take into account 'count' and 'offset' when displaying strings.

 0.1ss Aug 04, 2015
   Enable stack allocations code (using custom modified perflib).
   Sort objects in 'objects/' with default sort.

 0.1-stacks Aug 03, 2015
   Enable stack allocations code (using custom modified perflib).

 0.1 July 30, 2015
   Initial Release

