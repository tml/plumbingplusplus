# Plumbing++

Most programmers are familiar with the concept of a Unix pipe. Besides allowing
easy function composition, they provide an easy way to implicitly run each task
concurrently.  This library aims to do just that in C++, allowing for
type-safe, concurrent and parallelized pipelines of functions "for free".

## Synopsis (API Proposal)

As an example, say you have a nice set of composable functions, and you want to
feed data through them:

    Image<RGB> loadImage(std::string path);
    Image<RGB>& processImage(Image<RGB>& image);
    void saveImage(Image<RGB>const& image);

To process several images, the simple imperative approach is a for loop:

    std::vector<std::string> paths{"tree.jpg", "mountain.jpg", "car.jpg"};

    for (auto&& path : paths)
    {
        Image<RGB> image = loadImage(path);
        saveImage(processImage(image));
    }

It gets the job done but is single threaded and not concurrent. Most of the time
the program will be blocked, waiting for IO to complete. Since it is single
threaded, parallelism cannot be exploited. Wouldn't it be nice if one image was
being read, while another was being processed, while another was being saved?

Enter Plumbing++, which automatically creates threads for each processing step,
and joins each with a concurrent FIFO:

    
    std::vector<std:string> paths{"tree.jpg", "mountain.jpg", "car.jpg"};

    (paths >> loadImage >> processImage >> saveImage).wait();

Done! Each connection takes an iterable object, and returns the end of a
concurrent FIFO called a Sink:

    Plumbing::Sink<Image<RGB>> imageSink = (paths >> loadImage)

These can be freely iterated over, or used in STL algorithms:

    std::vector<Image<RGB>> imageCopies;
    std::copy(imageSink.begin(), imageSink.end(), std::back_inserter(imageCopies))

    for (auto&& elem : anotherSink)
    {
        std::cout << elem << std::endl;
    }

Each Sink (being iterable) thus becomes the input to the next
connection. If the last function in the pipeline returns void, the connect call
returns a future, so the caller can wait for the whole computation to complete:

    std::future<void> future = (paths >> loadImage >> processImage >> saveImage);
    future.wait();

For those opposed to operator overloading, a variadic function "connect" that does the same thing:

    Plumbing::connect(paths, loadImage, processImage, saveImage).wait();


## Progress/Flaws

The above features are currently implemented, but I am seeking constructive
criticism on the API and implementation. There is a reddit discussion thread
[here](http://www.reddit.com/r/programming/comments/14hgne/plumbing_small_library_that_automatically_makes/)
with lots of ideas and suggestions. Everything after this point is sort of a
"status report meets stream of conciousness" ramble.

Currently, several improvements can be made to the implementation regarding move
semantics, to save expensive copies.  It should be possible to move objects all
the way through the pipeline.

### Operator overloading

A concern with the API is the use of operator overloading. The operator &gt;&gt;
template function may accept too broad a range of inputs, and also leaks into
the global name space. Not cool.

As mentioned above, a variadic template function "connect" exists that avoids the problem of
operator overloading. As suggested in the reddit thread, the influence of operator &gt;&gt;
can be narrowed by only allowing it to act on a special object, requiring the user to create
that object to jump-start the process, e.g.:

    ( Plumbing::MakeSink(vals) >> getFirstChar >> printLine<char> ).wait();

This means operator &gt;&gt; would essentially only act on these Sinks:

    template <typename T, typename Func>
    // return type elided to spare you of ugly template voodoo
    operator >> (Sink<T>& sink, Func func);

This is nice, however this Sink type now has to wrap all types of iterable
objects (not just Pipes as it does currently). This can be done through type
erasure, but would incur an extra virtual method call for every dereference of
the underlying iterable (be it a Pipe&lt;T&gt;, or std::vector&lt;int&gt;).

The alternative is to have two distinct types:
 * A Sink&lt;T&gt; that encapsulates the end of Pipe&lt;T&gt;
 * An Input&lt;Container&lt;T&gt;&gt; that is a thin wrapper around an iterable
   object in order to restrict when operator &gt;&gt; gets triggered.

This is less elegant requiring more types and more functions definitions, but
will be more efficient, incurring no calls to virtual methods, while restricting
the use of operator &gt;&gt;

## Future Work/Ideas

This section covers some "TODOs", and things to look into.

### Connecting non-trivial functions

Single input, single output functions are a small subset of the functions one
would want to pipeline, so here are a few ideas on how to incorporate those into
the library as well.

First, functions that are "asymmetric" they require several items of the same
input type and produce only a single output (or vice versa). These can be defined
through iterators:

    // As an example, creates one image for every three,
    // effectively merging them
    template <typename InputIterator, typename OutputIterator>
    void mergeHDR(InputIterator& first,
                  InputIterator& last,
                  OutputIterator& dest);

    // pipeline saves a single merged image for every 3 paths given
    (paths >> loadImage >> mergeHDR >> saveImage).wait();

There might be problems automatically deducing whether a callable entity is the
first kind (one input, one output) vs the second kind (returns void, but works
on iterators). Template trickery will have to be employed.

Other functions could take several arguments of different types, and return
something else:

    /**
     * Takes a background, and a foreground with transparency and overlays the
     * two to make a new image
     */
    Image<RGB> overlay(Image<RGB>& background, Image<RGBA>& foregroundWithMask);

    Plumbing::combine(backgroundSink, foregroundSink) >> overlay;

### Other ways of connecting

User plhk in this [this comment on
reddit](http://www.reddit.com/r/programming/comments/14hgne/plumbing_small_library_that_automatically_makes/c7dp7x9)
suggests using the (&gt;&gt;=) operator to assemble the pipeline. This is very
fitting as it is the bind operator in Haskell (which this library is inspired
from), effectively porting Haskell syntax directly to C++. 

However since this is C++, the (&gt;&gt;=) operator is right associative, so at
present you would have to write:

    (((paths >>= loadImage) >>= processImage) >>= saveImage).wait();

Of course, this can be "remedied" by changing the semantics of the operator so
it aggregates functions from right to left until you finally bind to an
iterable, at which point it will trigger execution. This is useful in
"pre assembling" pipelines and then reusing/combining them later:

    pipeline = (loadImage >>= processImage >>= saveImage);
    (paths >>= pipeline);
    (otherPaths >>= pipeline);

This could also allow aggregation of the execution graph of the whole pipeline,
that can be used for task scheduling, if/when work-stealing is implemented.
