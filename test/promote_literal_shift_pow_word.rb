# Under --int-overflow=promote a shift, a power or an arithmetic operation
# whose operands are all literals is a Bignum when the result leaves the
# target's word. The check judged the word as 64 bits, so on a 32-bit target
# (wasm32-wasi, where Integer is 32 bits) `1 << 53` and `2**53` were typed
# Integer and raised RangeError, and `2.pow(70)` did not build anywhere.
C = 1 << 53
p C
D = 2**53
p D
def lim = 1 << 53
p lim
E = 2.pow(40)
p E
F = 100000 * 100000
p F
G = 1 << 31
p G
H = 1 << 30
p H
x = 1
p x << 53
p 9007199254740992
p(1 << 70)
p 2**70
p 2.pow(70)
p 3 * 2**62
