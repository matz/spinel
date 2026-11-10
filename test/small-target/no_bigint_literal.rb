# --no-bigint: a literal past sp_int can only be a Bignum, so it is refused
# at its own line rather than linking the bigint runtime.
x = 1 << 2
y = 123456789012345678901234567890
puts x + y
