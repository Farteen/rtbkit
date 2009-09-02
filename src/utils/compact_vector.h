/* compact_vector.h                                                -*- C++ -*-
   Jeremy Barnes, 3 March 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   An optimized class for holding a variable length unbounded vector.  If the
   vector is below a certain threshold, then the data will all be stored
   internally.  Otherwise, the data is stored externally and a pointer is
   kept.

   Conforms to the interface of std::vector and has iterator validity
   guarantees that are at least as strong.
*/

#ifndef __utils__compact_vector_h__
#define __utils__compact_vector_h__

#include <limits>
#include <vector>
#include "arch/exception.h"
#include "compiler/compiler.h"
#include <ostream>
#include <iterator>
#include <algorithm>

namespace ML {

template<typename Data,
         size_t Internal = 0,
         typename Size = uint32_t,
         bool Safe = true,
         typename Pointer = Data *,
         class Allocator = std::allocator<Data> >
class compact_vector {
public:
    typedef typename std::vector<Data>::iterator iterator;
    typedef typename std::vector<Data>::const_iterator const_iterator;
    typedef Pointer pointer;
    typedef Size size_type;
    typedef Data value_type;
    typedef Data & reference;
    typedef const Data & const_reference;
    
    compact_vector()
        : size_(0), is_internal_(true)
    {
#if 0
        using namespace std;
        cerr << __PRETTY_FUNCTION__ << endl;
        cerr << "sizeof(this) = " << sizeof(*this) << endl;
        cerr << "sizeof(Pointer) = " << sizeof(Pointer) << endl;
        cerr << "sizeof(Data) = " << sizeof(Data) << endl;
#endif
    }

    template<class ForwardIterator>
    compact_vector(ForwardIterator first,
                   ForwardIterator last)
        : size_(0), is_internal_(true)
    {
        init(first, last, std::distance(first, last));
    }

    ~compact_vector()
    {
        clear();
    }

    compact_vector(const compact_vector & other)
        : size_(0), is_internal_(true)
    {
        init(other.begin(), other.end(), other.size());
    }
    
    compact_vector & operator = (const compact_vector & other)
    {
        compact_vector new_me(other);
        swap(new_me);
        return *this;
    }

    void swap(compact_vector & other)
    {
        // Both external: easy case (swapping only)
        if (!(is_internal() || other.is_internal())) {
            swap_size(other);

            Size t = ext.capacity_;
            ext.capacity_ = other.ext.capacity_;
            other.ext.capacity_ = t;
            
            Pointer t2 = ext.pointer_;
            ext.pointer_ = other.ext.pointer_;
            other.ext.pointer_ = t2;

            return;
        }

        // Both internal: swap internal elements
        if (is_internal() && other.is_internal()) {

            // Swap common internal elements
            for (size_type i = 0;  i < size() && i < other.size();  ++i)
                std::swap(internal()[i], other.internal()[i]);

            // Copy leftovers
            for (size_type i = size();  i < other.size();  ++i) {
                new (internal() + i) Data(other.internal()[i]);
                other.internal()[i].~Data();
            }

            for (size_type i = other.size();  i < size();  ++i) {
                new (other.internal() + i) Data(internal()[i]);
                internal()[i].~Data();
            }
            
            swap_size(other);
            
            return;
        }

        // Do it the other way around
        if (!is_internal()) {
            other.swap(*this);
            return;
        }

        // This one is internal and the other is external.
        // We need to get the old pointer, then move over the internal
        // elements.

        Pointer p = other.ext.pointer_;
        Size capacity = other.ext.capacity_;

        other.is_internal_ = true;

        // Initialize and copy the internal elements for the other one
        for (size_type i = 0;  i < size();  ++i) {

            // NOTE: if the default constructor or the swap or the destructor
            // throws, we will have an invalid object.
            new (other.internal() + i) Data();
            std::swap(other.internal()[i], internal()[i]);
            internal()[i].~Data();
        }
        
        is_internal_ = false;
        swap_size(other);
        ext.pointer_ = p;
        ext.capacity_ = capacity;
    }

    void clear()
    {
        Data * p = data();
        for (size_type i = 0;  i < size_;  ++i)
            p[i].~Data();

        if (!is_internal()) {
            bool debug = false;
            using namespace std;
            if (debug) 
                cerr << "deallocating " << ext.capacity_ << " elements at "
                     << ext.pointer_ << " for " << this << endl;
            allocator.deallocate(ext.pointer_, ext.capacity_);
            is_internal_ = true;
        }

        size_ = 0;
    }
    
    void reserve(size_t new_capacity)
    {
        if (capacity() >= new_capacity) return;

        size_t to_alloc = std::max<size_t>(capacity() * 2, new_capacity);
        to_alloc = std::min<size_t>(to_alloc, max_size());

        compact_vector new_me(begin(), end(), to_alloc);
        swap(new_me);
    }

    void resize(size_t new_size, const Data & new_element = Data())
    {
        if (size_ == new_size) return;

        if (size_ < new_size) {
            // expand
            reserve(new_size);
            while (size_ < new_size) {
                new (data() + size_) Data(new_element);
                ++size_;
            }
            return;
        }

        // contract
        if  (!is_internal() && new_size <= Internal) {
            // Need to convert to internal representation
            compact_vector new_me(begin(), begin() + new_size,
                                  new_size);
            
            swap(new_me);
            return;
        }

        while (size_ > new_size) {
            data()[size_ - 1].~Data();
            --size_;
        }
    }
    
    void push_back(const Data & d)
    {
        if (size_ >= capacity())
            reserve(size_ * 2);

        new (data() + size_) Data(d);
        ++size_;
    }

    void pop_back()
    {
        if (Safe && empty())
            throw Exception("popping back empty compact vector");

#if 0 // save space when making smaller
        if (size_ == Internal + 1) {
            // Need to convert representation
            compact_vector new_me(begin(), begin() + Internal, Internal);
            swap(new_me);
            return;
        }
#endif

        data()[size_ - 1].~Data();
        --size_;
    }

    iterator insert(iterator pos, const Data & x)
    {
        return insert(pos, 1, x);
    }

    template <class ForwardIterator>
    iterator insert(iterator pos,
                    ForwardIterator f, ForwardIterator l)
    {
        size_type nelements = std::distance(f, l);

        iterator result = start_insert(pos, nelements);

        // copy the new elements
        std::copy(f, l, result);

        return result + nelements;
    }

    void insert(iterator pos, size_type n, const Data & x)
    {
        iterator result = start_insert(pos, n);

        std::fill(result, result + n, x);
        
        return result + n;
    }

    iterator erase(iterator pos)
    {
        return erase(pos, pos + 1);
    }

    iterator erase(iterator first, iterator last)
    {
        if (Safe) {
            if (first > last)
                throw Exception("compact_vector::erase(): last before first");
            if (first < begin() || last > end())
                throw Exception("compact_vector::erase(): iterators not ours");
        }

        int firstindex = first - begin();

        int n = last - first;

        if (n == 0) return first;

        size_t new_size = size_ - n;

        if (!is_internal() && new_size <= Internal) {
            /* If we become small enough to be internal, then we need to copy
               to avoid becoming smaller */
            compact_vector new_me(begin(), first, new_size);
            new_me.insert(new_me.end(), last, end());
            swap(new_me);
            return begin() + firstindex;
        }

        /* Move the elements (TODO: swap instead of copy) */
        std::copy(last, end(), first);

        /* Delete those at the end */
        while (size_ > new_size) {
            data()[size_ - 1].~Data();
            --size_;
        }

        return begin() + firstindex;
    }

    Data & operator [] (Size index)
    {
        if (Safe) check_index(index);
        return data()[index];
    }

    const Data & operator [] (Size index) const
    {
        if (Safe) check_index(index);
        return data()[index];
    }

    Data & at(Size index)
    {
        check_index(index);
        return data()[index];
    }

    const Data & at(Size index) const
    {
        check_index(index);
        return data()[index];
    }

    Data & front()
    {
        return operator [] (0);
    }

    const Data & front() const
    {
        return operator [] (0);
    }

    Data & back()
    {
        return operator [] (size_ - 1);
    }

    const Data & back() const
    {
        return operator [] (size_ - 1);
    }

    iterator begin() { return iterator(data()); }
    iterator end()   { return iterator(data() + size_); }
    const_iterator begin() const { return const_iterator(data()); }
    const_iterator end()const    { return const_iterator(data() + size_); }

    size_type size() const { return size_; }
    bool empty() const { return size_ == 0; }
    size_type capacity() const { return is_internal() ? Internal : ext.capacity_; }
    size_type max_size() const
    {
        return std::numeric_limits<Size>::max();
    }
    
private:
    struct {
        Size size_: 8 * sizeof(Size) - 1;
        Size is_internal_ : 1;
    } JML_PACKED;

    union {
        struct {
            char internal_[sizeof(Data) * Internal];
        } JML_PACKED itl;
        struct {
            Size capacity_;
            Pointer pointer_;
        } JML_PACKED ext;
    };
    
    bool is_internal() const { return is_internal_; }
    Data * internal() { return (Data *)(itl.internal_); }
    const Data * internal() const { return (Data *)(itl.internal_); }
    Data * data() { return is_internal() ? internal() : ext.pointer_; }
    const Data * data() const { return is_internal() ? internal() : ext.pointer_; }

    void check_index(size_type index) const
    {
        if (index >= size_)
            throw Exception("compact_vector: index out of range");
    }

    compact_vector(const_iterator first,
                   const_iterator last,
                   size_t to_alloc)
        : size_(0), is_internal_(true)
    {
        init(first, last, to_alloc);
    }

    template<class InputIterator>
    void init(InputIterator first, InputIterator last,
              size_t to_alloc)
    {
        clear();

        if (to_alloc > max_size())
            throw Exception("compact_vector can't grow that big");
        
        if (to_alloc > Internal) {
            is_internal_ = false;
            ext.pointer_ = allocator.allocate(to_alloc);
            ext.capacity_ = to_alloc;
        }
        else is_internal_ = true;
        
        Data * p = data();
        
        // Copy the objects across into the uninitialized memory
        for (; first != last;  ++first, ++p, ++size_) {
            if (Safe && size_ > to_alloc)
                throw Exception("compact_vector: internal logic error in init()");
            new (p) Data(*first);
        }
    }

    void swap_size(compact_vector & other)
    {
        Size t = size_;
        size_ = other.size_;
        other.size_ = t;
    }

    /** Insert n objects at position index */
    iterator start_insert(iterator pos, size_type n)
    {
        if (n == 0) return pos;

        int index = pos - begin();

        if (Safe && (index < 0 || index > size_))
            throw Exception("compact_vector insert: invalid index");

        using namespace std;
        bool debug = false;
        if (debug)
            cerr << "start_insert: index = " << index << " n = " << n
             << " size() = " << size() << " capacity() = "
                 << capacity() << endl;
        
        if (size() + n > capacity()) {
            reserve(size() + n);
            if (debug)
                cerr << "after reserve: index = " << index << " n = " << n
                     << " size() = " << size() << " capacity() = "
                     << capacity() << endl;
            
        }

        if (debug)
            cerr << "data() = " << data() << endl;

        // New element
        for (unsigned i = 0;  i < n;  ++i, ++size_) {
            if (debug)
                cerr << "i = " << i << " n = " << n << " size_ = " << size_
                     << endl;
            new (data() + size_) Data();
        }

        // Move elements to the end
        for (int i = size_ - 1;  i >= index + (int)n;  --i)
            data()[i] = data()[i - n];

        return begin() + index;
    }

    static Allocator allocator;
};

template<class Data, size_t Internal, class Size, bool Safe,
         class Pointer, class Allocator>
Allocator
compact_vector<Data, Internal, Size, Safe, Pointer, Allocator>::allocator;

template<class Data, size_t Internal, class Size, bool Safe,
         class Pointer, class Allocator>
bool
operator == (const compact_vector<Data, Internal, Size, Safe, Pointer, Allocator> & cv1,
             const compact_vector<Data, Internal, Size, Safe, Pointer, Allocator> & cv2)
{
    return cv1.size() == cv2.size()
        && std::equal(cv1.begin(), cv1.end(), cv2.begin());
}

template<class Data, size_t Internal, class Size, bool Safe,
         class Pointer, class Allocator>
bool
operator < (const compact_vector<Data, Internal, Size, Safe, Pointer, Allocator> & cv1,
             const compact_vector<Data, Internal, Size, Safe, Pointer, Allocator> & cv2)
{
    return std::lexicographical_compare(cv1.begin(), cv1.end(),
                                        cv2.begin(), cv2.end());
}

template<class Data, size_t Internal, class Size, bool Safe,
         class Pointer, class Allocator>
std::ostream &
operator << (std::ostream & stream,
             const compact_vector<Data, Internal, Size, Safe, Pointer, Allocator> & cv)
{
    stream << "{ ";
    std::copy(cv.begin(), cv.end(), std::ostream_iterator<Data>(stream, " "));
    return stream << " }";
}

template<typename D, size_t I, typename S, bool Sf, typename P, class A>
void make_vector_set(compact_vector<D, I, S, Sf, P, A> & vec)
{
    std::sort(vec.begin(), vec.end());
    vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
}

} // namespace ML

#endif /* __utils__compact_vector_h__ */

