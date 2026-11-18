# A send of const_get by a literal name reaches a constant by a name the
# program computes, so the lambda the constant holds starts where its write
# runs and runs any number of times.
L = -> { p :send }
Object.send(:const_get, :L).call
