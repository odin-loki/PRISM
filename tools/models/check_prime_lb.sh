#!/bin/bash
# Check that the generated _M_next_bkt equals the lower_bound original (result and
# _M_next_resize) on 0..2e5, 2^k +- 3 and every table prime +- 1.
# usage: check_prime_lb.sh <original header> <new header>
set -e; d=$(mktemp -d)
for x in o n; do h=$1; [ $x = n ] && h=$2
cat > $d/$x.cpp <<E
#include "$(realpath $h)"
#include <cstdio>
int main(){ std::_Prism_hash_detail::_Prism_prime_rehash_policy p;
 auto f=[&](unsigned long x){ auto r=p._M_next_bkt(x); std::printf("%lu %lu %lu\n",x,r,(unsigned long)p._M_state()); };
 for(unsigned long n=0;n<200000;n++) f(n);
 for(int s=0;s<64;s++) for(long k=-3;k<=3;k++) f((1ul<<s)+k);
 auto L=std::_Prism_hash_detail::_S_prime_list(); for(int i=0;i<304;i++) for(long k=-1;k<=1;k++) f(L[i]+k);
}
E
clang++-18 -std=c++23 -O1 $d/$x.cpp -o $d/$x && $d/$x > $d/$x.txt; done
cmp $d/o.txt $d/n.txt && echo "IDENTICAL ($(wc -l < $d/o.txt) inputs)"; rm -rf $d
