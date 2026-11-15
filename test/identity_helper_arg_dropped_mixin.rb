# A helper that a module brings in by include keeps the table it is given:
# the table stays boxed and answers as CRuby does. Two checks decline it:
# the include makes the program-wide name check (an_prog_never_gives) answer
# no for every name, and the include's copy of the def has a parameter the
# def's own nodes do not read.
# spinel: gc-stress

# a helper an include copies into the class
module Keeper
  def keep_inc(o)
    @kept_inc = o
    o
  end
end

class Inc
  include Keeper

  def run
    in_t = Array.new(2) { Array.new(2, 0) }
    keep_inc(in_t)
    @kept_inc[0][0] = "i"
    p in_t[0]
  end
end
Inc.new.run

p Inc.new.keep_inc(2)
