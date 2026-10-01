# Enumerable, in Ruby, compiled by spinel with the program that uses it.
#
# Each method here is the one definition of that name: the compiler has no
# C emitter and no runtime arm for it. A program that mentions one of these
# names has this file spliced ahead of it (spinel_parse.c), and the analyzer
# rewrites every definition into a top-level function that takes the
# receiver as its first parameter, `self` and the receiverless calls in the
# body becoming that parameter (analyze_desugar.c, desugar_builtins). A call
# `recv.m(args) { }` on an Array, a Hash, a Range, an Enumerator, a class
# that includes Enumerable, or a value only known at run time is rewritten
# into `__enum_m(recv, args) { }`, which the inliner then specializes for
# the receiver's type at that call site, exactly as it does for a yielding
# method the program wrote itself. A method that is never called never
# reaches the generated C.
#
# Write these in the Ruby the compiler compiles: `each` and `yield`, plain
# locals, no reflection. A body that needs an operation Ruby cannot express
# is a case for a C emitter, not for an intrinsic.
#
# A method that answers an Enumerator when called without a block says so
# with `if block_given?` as its last statement, the block arm first: the
# analyzer types a call with a block from that arm and a call without one
# from the other, and the inliner keeps only the arm the call site takes.
module Enumerable
  def each_with_object(memo)
    if block_given?
      each { |x| yield x, memo }
      memo
    else
      __enum_pairs(map { |x| [x, memo] })
    end
  end

  def tally(hash = nil)
    __no_false(hash, "Hash")
    if hash
      each { |x| hash[x] = hash.fetch(x, 0) + 1 }
      hash
    else
      counts = {}
      each { |x| counts[x] = counts.fetch(x, 0) + 1 }
      counts
    end
  end

  # The first round walks the receiver with its own `each`, keeping what it
  # yields; the later rounds replay those, as CRuby's does. The blockless
  # form answers an Enumerator the emitter builds (#first(n) on an endless
  # one is folded there); the rewrite leaves it there.
  def cycle(n = nil)
    if block_given?
      forever = true
      rounds = 0
      __no_false(n, "Integer")
      if n
        raise TypeError, "no implicit conversion of #{n.class} into Integer" unless n.is_a?(Integer) || n.is_a?(Float)
        forever = false
        rounds = n.to_i
      end
      if self.is_a?(Array)
        # an Array is walked again each round, as Array#cycle does (a
        # change to it between rounds shows)
        unless empty?
          r = 0
          while forever || r < rounds
            each { |x| yield x }
            r += 1
          end
        end
      elsif forever || rounds > 0
        seen = []
        each do |x|
          seen << x
          yield x
        end
        unless seen.empty?
          r = 1
          while forever || r < rounds
            seen.each { |x| yield x }
            r += 1
          end
        end
      end
      nil
    else
      each
    end
  end

  def partition
    if block_given?
      yes = []
      no = []
      each { |x| (yield(x) ? yes : no) << x }
      [yes, no]
    else
      each
    end
  end

  def group_by
    if block_given?
      groups = {}
      each { |x| (groups[yield(x)] ||= []) << x }
      groups
    else
      each
    end
  end

  def min_by(n = nil)
    if block_given?
      __no_false(n, "Integer")
      if n
        raise ArgumentError, "negative size (#{n})" if n < 0
        sort_by { |x| yield x }.first(n)
      else
        # one walk: the first element's key is taken as it comes, so a
        # receiver that yields once is not read twice, and a leading nil is
        # an element like any other
        best = nil
        best_key = nil
        found = false
        each do |x|
          key = yield x
          if found
            c = key <=> best_key
            raise ArgumentError, "comparison of #{key.class} with #{(best_key.nil? || best_key == true || best_key == false || best_key.is_a?(Numeric) || best_key.is_a?(Symbol)) ? best_key.inspect : best_key.class} failed" if c.nil?
            if c < 0
              best = x
              best_key = key
            end
          else
            best = x
            best_key = key
            found = true
          end
        end
        best
      end
    else
      each
    end
  end

  def max_by(n = nil)
    if block_given?
      __no_false(n, "Integer")
      if n
        raise ArgumentError, "negative size (#{n})" if n < 0
        # descending by key, ties in encounter order: a stable ascending sort
        # of the reversed elements, read backwards
        to_a.reverse.sort_by { |x| yield x }.reverse.first(n)
      else
        # one walk: the first element's key is taken as it comes, so a
        # receiver that yields once is not read twice, and a leading nil is
        # an element like any other
        best = nil
        best_key = nil
        found = false
        each do |x|
          key = yield x
          if found
            c = key <=> best_key
            raise ArgumentError, "comparison of #{key.class} with #{(best_key.nil? || best_key == true || best_key == false || best_key.is_a?(Numeric) || best_key.is_a?(Symbol)) ? best_key.inspect : best_key.class} failed" if c.nil?
            if c > 0
              best = x
              best_key = key
            end
          else
            best = x
            best_key = key
            found = true
          end
        end
        best
      end
    else
      each
    end
  end

  def minmax_by
    if block_given?
      min = nil
      max = nil
      min_key = nil
      max_key = nil
      found = false
      each do |x|
        key = yield x
        if found
          c = key <=> min_key
          raise ArgumentError, "comparison of #{key.class} with #{(min_key.nil? || min_key == true || min_key == false || min_key.is_a?(Numeric) || min_key.is_a?(Symbol)) ? min_key.inspect : min_key.class} failed" if c.nil?
          if c < 0
            min = x
            min_key = key
          end
          c = key <=> max_key
          raise ArgumentError, "comparison of #{key.class} with #{(max_key.nil? || max_key == true || max_key == false || max_key.is_a?(Numeric) || max_key.is_a?(Symbol)) ? max_key.inspect : max_key.class} failed" if c.nil?
          if c > 0
            max = x
            max_key = key
          end
        else
          min = x
          max = x
          min_key = key
          max_key = key
          found = true
        end
      end
      [min, max]
    else
      each
    end
  end

  def minmax
    # Unlike minmax_by (whose blockless arm has no key function to apply, so
    # it answers an Enumerator), minmax always computes immediately: with a
    # block, the block IS the comparator (`yield(a, b)`, not `a <=> b`);
    # without one, `<=>` is. Neither arm can be `each` (an Enumerator).
    #
    # One walk with a flag, as min_by and max_by take theirs: a leading nil is
    # an element like any other (`[nil, 3].minmax` raises), and only an empty
    # receiver answers [nil, nil].
    min = nil
    max = nil
    found = false
    if block_given?
      each do |x|
        if found
          c = yield(x, min)
          raise ArgumentError, "comparison of #{min.class} with #{(x.nil? || x == true || x == false || x.is_a?(Numeric) || x.is_a?(Symbol)) ? x.inspect : x.class} failed" if c.nil?
          min = x if c < 0
          c = yield(x, max)
          raise ArgumentError, "comparison of #{max.class} with #{(x.nil? || x == true || x == false || x.is_a?(Numeric) || x.is_a?(Symbol)) ? x.inspect : x.class} failed" if c.nil?
          max = x if c > 0
        else
          min = x
          max = x
          found = true
        end
      end
      [min, max]
    else
      each do |x|
        if found
          c = x <=> min
          # CRuby's own message names the ACCUMULATOR's class unconditionally
          # first, the new element's class or inspect second (the mirror of
          # min_by/max_by's message, which names the new element first) --
          # verified against `[1, "a"].minmax` ("comparison of Integer with
          # String failed") and `["a", 1].minmax` ("comparison of String
          # with 1 failed").
          raise ArgumentError, "comparison of #{min.class} with #{(x.nil? || x == true || x == false || x.is_a?(Numeric) || x.is_a?(Symbol)) ? x.inspect : x.class} failed" if c.nil?
          min = x if c < 0
          c = x <=> max
          raise ArgumentError, "comparison of #{max.class} with #{(x.nil? || x == true || x == false || x.is_a?(Numeric) || x.is_a?(Symbol)) ? x.inspect : x.class} failed" if c.nil?
          max = x if c > 0
        else
          min = x
          max = x
          found = true
        end
      end
      [min, max]
    end
  end

  def filter_map
    if block_given?
      out = []
      each do |x|
        v = yield x
        out << v if v
      end
      out
    else
      each
    end
  end

  def flat_map
    if block_given?
      out = []
      each do |x|
        v = yield x
        if v.is_a?(Array)
          out.concat(v)
        else
          out << v
        end
      end
      out
    else
      each
    end
  end

  def count
    if block_given?
      n = 0
      each { |x| n += 1 if yield(x) }
      n
    else
      # unreached by the rewrite (desugar_builtin_enum_calls keeps a
      # blockless, argumentless `count` on its typed size emitter), kept
      # correct here for the same reason Enumerable#count itself walks
      # `each` rather than assuming a `size` method exists.
      n = 0
      each { |x| n += 1 }
      n
    end
  end

  def any?
    if block_given?
      found = false
      each do |x|
        if yield(x)
          found = true
          break
        end
      end
      found
    else
      # unreached by the rewrite (desugar_builtin_enum_calls keeps a
      # blockless call, and the pattern-argument form, on the typed
      # emitter): a blockless any? asks about each element's own
      # truthiness, not the block's.
      found = false
      each do |x|
        if x
          found = true
          break
        end
      end
      found
    end
  end

  def all?
    if block_given?
      result = true
      each do |x|
        unless yield(x)
          result = false
          break
        end
      end
      result
    else
      result = true
      each do |x|
        unless x
          result = false
          break
        end
      end
      result
    end
  end

  def none?
    if block_given?
      result = true
      each do |x|
        if yield(x)
          result = false
          break
        end
      end
      result
    else
      result = true
      each do |x|
        if x
          result = false
          break
        end
      end
      result
    end
  end

  def one?
    if block_given?
      n = 0
      each do |x|
        n += 1 if yield(x)
        break if n > 1
      end
      n == 1
    else
      n = 0
      each do |x|
        n += 1 if x
        break if n > 1
      end
      n == 1
    end
  end

  def find
    if block_given?
      found = nil
      each do |x|
        if yield(x)
          found = x
          break
        end
      end
      found
    else
      each
    end
  end

  def find_index
    # An accumulator + break, not `return i if yield(x)`: a `return` inside
    # a block only reaches out of an INLINED copy of this method (a plain C
    # goto within one function). A receiver known only at run time (a poly
    # value, or one an instantiated class might itself answer `each` for)
    # is walked through the generic, non-inlined clone instead, where the
    # block is a real materialized Proc called across an actual function
    # boundary that a goto cannot cross -- `return` silently landed back in
    # the clone's own unconditional trailing `nil` no matter what the block
    # found. `break`'s non-local exit is built to cross that boundary (the
    # same mechanism find/min_by/any? already rely on), so it is exit this
    # is written in.
    if block_given?
      idx = nil
      i = 0
      each do |x|
        if yield(x)
          idx = i
          break
        end
        i += 1
      end
      idx
    else
      # unreached by the rewrite (desugar_builtin_enum_calls keeps the
      # value-argument form `find_index(v)` and the blockless Enumerator
      # form on the emitter); kept correct here for the same reason the
      # other blockless arms are.
      idx = nil
      i = 0
      each do |x|
        if x
          idx = i
          break
        end
        i += 1
      end
      idx
    end
  end

  def each_with_index
    if block_given?
      i = 0
      each do |x|
        yield x, i
        i += 1
      end
      self
    else
      # A fresh `recv`/`j`, not the `if` arm's `self`/`i`: CRuby scopes a
      # method's locals across its whole body, not per `if`/`else` branch, so
      # reusing those names here would make this branch's Enumerator.new
      # capture the SAME method-level locals the `if` arm also assigns --
      # one shared clone scope, one is_cell decision per name, so the capture
      # this branch needs (a heap cell, read through a pointer) would leak
      # into the `if` arm's plain, uncaptured use of them too, even though
      # the two arms never both run for one call. Measured: with the names
      # shared, a typed Array's block form (the hot arm) cost 2-3x an
      # ordinary loop (a fresh GC-allocated cell per outer iteration).
      recv = self
      Enumerator.new do |y|
        j = 0
        recv.each do |x|
          y << [x, j]
          j += 1
        end
      end
    end
  end

  def take_while
    if block_given?
      out = []
      each do |x|
        break unless yield x
        out << x
      end
      out
    else
      each
    end
  end

  def drop_while
    if block_given?
      out = []
      dropping = true
      each do |x|
        dropping = false if dropping && !yield(x)
        out << x unless dropping
      end
      out
    else
      each
    end
  end

  # inject and reduce are the same method under two names in CRuby (an
  # `alias`, not a delegation, so overriding one leaves the other alone) --
  # written here as two independent definitions rather than one canonicalized
  # to the other, the same way a user class's own override of just one name
  # leaves the other on Enumerable. Only the arity-0 block form
  # (`inject { |acc, x| ... }`) is a rewrite target: the seeded form
  # (`inject(seed) { }`), the symbol forms (`inject(:+)`, `inject(seed, :+)`)
  # and the bare argless call all have no parameter here, so they stay on the
  # existing arity-checked C emitter (desugar_builtin_enum_calls), the same
  # carve-out find_index/count have for the forms their definitions likewise
  # do not cover.
  def inject
    if block_given?
      acc = first
      skip = true
      each do |x|
        if skip
          skip = false
        else
          acc = yield(acc, x)
        end
      end
      acc
    else
      # unreached by the rewrite (desugar_builtin_enum_calls keeps every
      # blockless call -- the seeded/symbol forms and the bare argless call
      # alike -- on the existing emitter, which already raises this);
      # kept correct here for the same reason the other blockless arms are.
      raise ArgumentError, "wrong number of arguments (given 0, expected 1..2)"
    end
  end

  def reduce
    if block_given?
      acc = first
      skip = true
      each do |x|
        if skip
          skip = false
        else
          acc = yield(acc, x)
        end
      end
      acc
    else
      raise ArgumentError, "wrong number of arguments (given 0, expected 1..2)"
    end
  end

  # Unlike most Enumerable methods here, grep/grep_v are never an Enumerator
  # blockless: both arms compute immediately, `pattern === x` deciding
  # membership and the block (when given) transforming what's kept. Written
  # against `each` and a plain `===`, so any pattern CRuby's `===` accepts
  # (a class, a Range, a Regexp, a value compared by `==`, an object
  # defining its own `===`) works the same way here, without a per-pattern
  # C fold to keep in step.
  def grep(pattern)
    out = []
    if block_given?
      each { |x| out << yield(x) if pattern === x }
    else
      each { |x| out << x if pattern === x }
    end
    out
  end

  def grep_v(pattern)
    out = []
    if block_given?
      each { |x| out << yield(x) unless pattern === x }
    else
      each { |x| out << x unless pattern === x }
    end
    out
  end

  # An optional argument is absent only when it is nil: false is one, which
  # CRuby refuses. The test is on truthiness, so an omitted argument folds
  # it away.
  def __no_false(opt, what)
    raise TypeError, "no implicit conversion of false into #{what}" if !opt && !opt.nil?
  end
end
