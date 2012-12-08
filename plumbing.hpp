// ideas for "couplings" and fifos between transformations

#ifndef PLUMBING_H_IEGJRLCP
#define PLUMBING_H_IEGJRLCP

#include <cassert>

#include <vector>
#include <mutex>
#include <condition_variable>

#include <iterator>
#include <memory>
#include <functional>
#include <type_traits>
#include <thread>
#include <future>

#include <utility> // std::forward

#include <boost/optional.hpp>
#include <boost/none.hpp>

// used if number of inputs/outputs is not one-to-one.
// e.g. 3 images in, 1 image out (hdr)
// e.g. take two numbers and sum
/*
template <typename InIter, typename OutIter>
void iteratorTransformation(InIter first, InIter last, OutIter out);
*/

// perhaps restrict on postcrement?
/*
void sumTwo(int* first, int* last, int* out)
{
    while( first != last )
    {
        *out = *first++;
        *out += *first++;
        
        ++out;
    }
}
*/

// used when transformation creates a signle output from a single output.
/*
template <typename InType, typename OutType>
OutType tranformation(InType const& in);
*/

// is a transformation
/*
float convertToFloat(int in)
{
    return static_cast<float>(in);
}
*/

namespace Plumbing
{
    template <typename T>
    class Pipe
    {
        // TODO: dynamically resize fifo according to demand?

        // TODO: perhaps create a different queue which is "infinite" (a linked list),
        //       but provides a way to "stall" it on a predicate (e.g. memory usage)

        std::vector<boost::optional<T>> fifo_;
        int write_;
        int read_;
        std::mutex mutex_;
        std::condition_variable readyForWrite_;
        std::condition_variable readyForRead_;
        bool open_;

        /**
         * Return the number of free slots available for writing
         */
        inline int writeHeadroom()
        {
            return (write_ < read_
                   ? read_ - write_
                   : (read_ + fifo_.size()) - write_) - 1;
        }

        /**
         * Return the number of free slots available for reading
         */
        inline int readHeadroom()
        {
            return read_ <= write_
                   ? write_ - read_
                   : (write_ + fifo_.size()) - read_;
        }

        inline void incrementWrite()
        {
            write_ = (write_ + 1) % fifo_.size();
        }

        inline void incrementRead()
        {
            read_ = (read_ + 1) % fifo_.size();
        }


    public:
        Pipe(std::size_t fifoSize = 2)
            : fifo_(fifoSize),
              write_(0),
              read_(0),
              open_(true)
        {
            assert (fifoSize >= 2);
        }

        Pipe(Pipe<T> const& other) = delete;

        Pipe(Pipe<T>&& other) :
            fifo_(std::move(other.fifo_)),
            write_(std::move(other.write_)),
            read_(std::move(other.read_)),
            mutex_(),
            readyForWrite_(),
            readyForRead_(),
            open_(std::move(other.open_))
        {
            other.open_ = false;
        }

        Pipe<T>& operator = (Pipe<T>&& other)
        {
            fifo_ = std::move(other.fifo_);
            write_ = std::move(other.write_);
            read_ = std::move(other.read_);
            open_ = std::move(other.open_);
            other.open_ = false;

            return *this;
        }

        /************************************
         *  Facilities for writing to pipe  *
         ************************************/
        
        void enqueue(T const& e)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while(!writeHeadroom())
            {
                readyForWrite_.wait(lock);
            }

            fifo_[write_] = e;
            incrementWrite();

            readyForRead_.notify_one();
        }

        // TODO: have to look out for trying to enque after closing the pipe is closed
        // perhaps throw exception?
        void close()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while(!writeHeadroom())
            {
                readyForWrite_.wait(lock);
            }

            fifo_[write_] = boost::optional<T>(boost::none);
            incrementWrite();

            readyForRead_.notify_one();
        }

        /**************************************
         *  Facilities for reading from pipe  *
         **************************************/
        
        bool isOpen()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while(!readHeadroom())
            {
                readyForRead_.wait(lock);
            }

            return fifo_[read_];
        }

        T dequeue()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while(!readHeadroom())
            {
                readyForRead_.wait(lock);
            }

            // TODO std::move
            T const& e = *fifo_[read_];
            incrementRead();

            readyForWrite_.notify_one();

            return std::move(e);
        }
    };

    template <typename T>
    class Sink
    {
        std::shared_ptr<Pipe<T>> pipe_;

    public:
        typedef T value_type;
        typedef T* pointer;
        typedef T& reference;
        typedef Sink<T> iterator;
        typedef std::input_iterator_tag iterator_category;
        typedef void difference_type; // TODO: is this ok?

        Sink(Sink<T> const& other) = default;
        Sink(Sink<T>&& other) = default;

        Sink(std::shared_ptr<Pipe<T>> const& pipe)
            : pipe_(pipe)
        { }

        /**
         * Default constructor, creates an "end" iterator
         */
        Sink() : pipe_(nullptr) { }

        iterator& begin() { return *this; }
        iterator  end()   { return iterator(); }

        iterator& operator ++ ()    { return *this; } ///< noop
        iterator& operator ++ (int) { return *this; } ///< noop

        /**
         * To fullfil the input_iterator category, both returns the 
         * the next element and advances the inner iterator
         */
        value_type operator * ()    { return pipe_->dequeue(); }

        bool operator == (iterator& other)
        {
            Pipe<T>* a = this->pipe_.get();
            Pipe<T>* b = other.pipe_.get();
            
            if (a == b)
            {
                return true;
            }

            if (!a)
            {
                std::swap(a, b);
            }

            // an "end" iterator is:
            // - either the default constructed iterator (pipe_ is nullptr)
            // - or has reached the end of iteration (isOpen() returns false)
            return !(b || a->isOpen());
        }

        bool operator != (iterator& other) { return !(*this == other); }
    };

    namespace detail
    {

        // creating a struct to specialize template
        template <typename Output>
        struct connectImpl
        {

            template <typename InputIterable, typename Func>
            static Sink<Output> connect(InputIterable&& input, Func func)
            {
                std::shared_ptr<Pipe<Output>> pipe(new Pipe<Output>(2)); // TODO make this customizable

                // start processing thread
                std::thread processingThread(
                        [pipe, &input, func]()
                        {
                            for (auto&& e : input) {
                                pipe->enqueue(func(e));
                            }
                            pipe->close();
                        }
                        );

                processingThread.detach(); // TODO: shouldn't detach?

                return Sink<Output>(pipe);
            }
        };

        template <>
        struct connectImpl<void>
        {

            /**
             * Specialization for functions returning void.
             *
             * TODO: have to return an object ( a future? ) that allows
             * us to wait for the computation.
             */
            template <typename InputIterable, typename Func>
            static std::future<void> connect(InputIterable&& input, Func func)
            {
                // start processing thread
                return std::async(std::launch::async,
                        [&input, func]()
                        {
                            for (auto&& e : input) {
                                func(e);
                            }
                        }
                );
            }
        };

    }

    // TODO: make connect a member function of sink
    /**
     * @note need to specialize based on output of the function passed in,
     * so need another layer of indirection to connectImpl.
     */
    template <typename InputIterable, typename Func>
    auto connect(InputIterable&& input, Func func)
    -> decltype(
            detail::connectImpl<
                decltype(func(std::declval<typename std::remove_reference<InputIterable>::type::iterator::value_type>()))
            >::connect(std::forward<InputIterable>(input), func)
       )
    {
        typedef decltype(
                    func(
                        std::declval<typename std::remove_reference<InputIterable>::type::iterator::value_type>()
                    )
                ) Output;

        return detail::connectImpl<Output>::connect(std::forward<InputIterable>(input), func);
    }

    /**
     * @note: Perhaps this operator is too generically templated, and would "poison"
     * the code it is imported into? Using a connect function doesn't seem as
     * "slick"
     */
    template <typename InputIterable, typename Func>
    inline auto operator >> (InputIterable&& input, Func func)
    -> decltype(connect(std::forward<InputIterable>(input), func))
    {
        return connect(std::forward<InputIterable>(input), func);
    }

}

#endif /* end of include guard: PLUMBING_H_IEGJRLCP */
