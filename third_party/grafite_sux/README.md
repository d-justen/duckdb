Sux (fork) v1.0.3
=========

This is a fork of the Sux library, which is a C++ library for succinct data structures. The original library can be found at
this [link](https://github.com/vigna/sux).

This fork adapts the Elias-Fano data structure for its use in [Grafite](https://github.com/marcocosta97/grafite) as follows:
- adding a new constructor for Elias-Fano data structures that takes as input a pair of iterators to the ordered
positions in the bitvector that are set to 1
- remove the `select` methods from the `EliasFano` class, since they are not needed for our use of the data structure
- add a new method `rankv2` to the `EliasFano` class, which implements binary search of elements in a bucket
- clean up the code and remove unused methods

Licensing
---------

Sux is licensed exactly like `libstdc++` (GPLv3 + GCC Runtime Library
Exception), which essentially means you can use it everywhere, exactly
like `libstdc++`.