# bisect_search.rb -- the search `spinel bisect` runs over the decision keys
# of a compile (src/decide.c), apart from everything that builds or runs a
# program, so that it is tested as a unit (test/tools_bisect_search.rb).
# Written in the spinel subset.
#
# A key is `kind@site`. The search is handed the keys of the unrestricted
# compile, which is bad, and a probe: an object whose probe(keys) says what
# the program does when exactly `keys` are allowed. It answers a smallest
# set it can find that is still bad on its own.
#
# The search is QuickXplain (Junker 2004), not a binary search: a binary
# search finds one culprit and is wrong when two decisions are only bad
# together. It halves the keys and asks about one half at a time, keeping
# what a half needed while it looks in the other, so one culprit among N
# keys costs about log2 N builds and each further one about as much again.
# It leans on a wrong answer staying wrong when more is allowed, which is
# how decisions nearly always behave and not something they must: what it
# found is therefore built on its own, each key it does not need is dropped
# by a build, and when it is not bad on its own delta debugging (ddmin, over
# the kinds and then over the keys of the kinds left) starts again from the
# log. Two decisions that are only bad together are often two about one
# method, so the partner of a key that is not bad alone is looked for first
# among the keys at its own place. Every set tried is a subset of the log.
#
# A wrong answer staying wrong when more is allowed also answers questions
# without a build: a set that holds a bad one is taken as bad, a part of a
# good one as good. That steers the search and nothing else.

BS_GOOD = 0
BS_BAD = 1
BS_SKIP = 2   # the subset could not be judged (it did not build)

# The kind of a key: what stands before the first `@`.
def bs_kind(key)
  i = key.index("@")
  i ? key[0, i] : key
end

# The site of a key: the rest. A position, a method, `main`, or `-`.
def bs_site(key)
  i = key.index("@")
  i ? key[i + 1, key.length - i - 1] : ""
end

# The kinds of `keys`, each once, in the order they first appear.
def bs_kinds(keys)
  seen = {}
  out = []
  keys.each do |k|
    kd = bs_kind(k)
    if !seen.key?(kd)
      seen[kd] = true
      out.push(kd)
    end
  end
  out
end

# The keys of `keys` whose kind is one of `kinds`, in their own order.
def bs_of_kinds(keys, kinds)
  want = {}
  kinds.each { |kd| want[kd] = true }
  out = []
  keys.each { |k| out.push(k) if want.key?(bs_kind(k)) }
  out
end

# Where a key's decision was taken, coarsely: its method, or its file and
# line. Two decisions that are only bad together are often at one place (a
# method's root frame and the save that pops it). A position is
# file:line:col, and what follows a method's `:` is a name, never a number.
def bs_place(key)
  parts = bs_site(key).split(":")
  return parts[0] if parts.length < 2 || parts[1] != parts[1].to_i.to_s
  parts[0] + ":" + parts[1]
end

# The keys of `keys` at the place of one of `of`.
def bs_at_places(keys, of)
  at = {}
  of.each { |k| at[bs_place(k)] = true }
  out = []
  keys.each { |k| out.push(k) if at.key?(bs_place(k)) }
  out
end

# Is every key of `keys` a key of the hash `set`?
def bs_all_in(keys, set)
  i = 0
  while i < keys.length
    return false if !set.key?(keys[i])
    i += 1
  end
  true
end

# What a set of keys is remembered by, whatever order it comes in.
def bs_sig(keys)
  keys.sort.join("\n")
end

# `keys` without the ones in `drop`.
def bs_without(keys, drop)
  gone = {}
  drop.each { |k| gone[k] = true }
  out = []
  keys.each { |k| out.push(k) if !gone.key?(k) }
  out
end

class BisectSearch
  # probes made (an answer already known costs none) and how many of them
  # could not be judged
  attr_reader :calls, :skips

  def initialize(probe)
    @probe = probe
    @memo = {}
    @bad = []
    @good = []
    @above = []
    @within = []
    @jump = []
    @calls = 0
    @skips = 0
  end

  # An answer the caller already has.
  def know(keys, answer)
    @memo[bs_sig(keys)] = answer
    @bad.push(keys) if answer == BS_BAD
    @good.push(keys) if answer == BS_GOOD
  end

  def ask(keys)
    sig = bs_sig(keys)
    return @memo[sig] if @memo.key?(sig)
    r = @probe.probe(keys)
    @calls += 1
    @skips += 1 if r == BS_SKIP
    know(keys, r)
    r
  end

  # ask, but what follows from an answer already had is not built: a set
  # that holds a bad one is taken as bad, and a part of a good one as good.
  # That is how decisions nearly always behave and not something they must,
  # so only the search is steered by it: what run returns it has built.
  def suppose(keys)
    sig = bs_sig(keys)
    return @memo[sig] if @memo.key?(sig)
    have = {}
    keys.each { |k| have[k] = true }
    i = 0
    while i < @bad.length
      return BS_BAD if bs_all_in(@bad[i], have)
      i += 1
    end
    i = 0
    while i < @good.length
      if @good[i].length >= keys.length
        within = {}
        @good[i].each { |k| within[k] = true }
        return BS_GOOD if bs_all_in(keys, within)
      end
      i += 1
    end
    ask(keys)
  end

  # What a candidate stands for: its own keys, or, when the candidates are
  # kinds, the keys of `pool` of those kinds.
  def keys_of(items, pool)
    pool.length > 0 ? bs_of_kinds(pool, items) : items
  end

  # ddmin: shrink `items` while what is left is still bad, first to one of n
  # chunks, then to all but one of them, then with n doubled. The items are
  # keys, or kinds of the keys in `pool`.
  def ddmin(items, pool)
    n = 2
    while items.length >= 2
      size = (items.length + n - 1) / n
      reduced = false
      start = 0
      while start < items.length && !reduced
        chunk = items[start, size]
        if ask(keys_of(chunk, pool)) == BS_BAD
          items = chunk
          n = 2
          reduced = true
        end
        start += size
      end
      # at n == 2 the rest of one chunk is the other chunk, just tried
      start = 0
      while n > 2 && start < items.length && !reduced
        rest = []
        k = 0
        while k < items.length
          rest.push(items[k]) if k < start || k >= start + size
          k += 1
        end
        if rest.length > 0 && ask(keys_of(rest, pool)) == BS_BAD
          items = rest
          n -= 1
          reduced = true
        end
        start += size
      end
      if !reduced
        break if n >= items.length
        n *= 2
        n = items.length if n > items.length
      end
    end
    items
  end

  # QuickXplain (Junker 2004): the part of `cand` a smallest bad set needs,
  # given that `held` is allowed throughout and held + cand is bad. `grew`
  # says `held` was added to since it was last known not to be bad. `sure`
  # is the part of `held` already found to be needed: as soon as it is bad
  # with what the second half gave and nothing else, the first half is not
  # looked at, which is what makes one culprit cost one build per halving.
  def explain(held, sure, grew, cand)
    return [] if grew && suppose(held) == BS_BAD
    return cand if cand.length == 1
    mid = cand.length / 2
    first = cand[0, mid]
    second = cand[mid, cand.length - mid]
    @above.push(held)
    from_second = explain(held + first, sure, true, second)
    @above.pop
    return [] if @jump.length > 0
    if from_second.length > 0
      part = sure + from_second
      return from_second if suppose(part) == BS_BAD
      # what was found is not bad alone. Its partner is looked for first
      # among the keys at the same place: when it is bad with those, the
      # search starts again over them and nothing else
      near = bs_at_places(bs_without(@within, part), from_second)
      if near.length > 0 && part.length + near.length < @within.length && suppose(part + near) == BS_BAD
        @jump = part + near
        return []
      end
      settle(from_second, held)
    end
    # the frames above are waiting on this half, not on what is found in it
    above = @above
    @above = []
    from_first = explain(held + from_second, sure + from_second, from_second.length > 0, first)
    @above = above
    from_first + from_second
  end

  # `part` is needed and is not bad alone. This frame and each frame above
  # it is about to ask whether what it holds is enough with `part`, without
  # its own first half, and what a frame holds shrinks on the way up: the
  # answers run bad, ..., bad, good. The turn is found by halving, and the
  # frames then have their answers from what is known.
  def settle(part, held)
    lo = 0
    hi = @above.length + 1
    while lo < hi
      mid = (lo + hi) / 2
      h = mid < @above.length ? @above[mid] : held
      if suppose(h + part) == BS_BAD
        hi = mid
      else
        lo = mid + 1
      end
    end
  end

  # `found` is bad: drop each key it is still bad without. What is left
  # is bad and no key can be taken from it, which is all ddmin promises
  # too. One culprit costs nothing here (without it nothing is left).
  def trim(found)
    i = 0
    while i < found.length && found.length > 1
      rest = bs_without(found, [found[i]])
      if ask(rest) == BS_BAD
        found = rest
      else
        i += 1
      end
    end
    found
  end

  # `items` in the order of `order`.
  def in_order(items, order)
    have = {}
    items.each { |k| have[k] = true }
    out = []
    order.each { |k| out.push(k) if have.key?(k) }
    out
  end

  # QuickXplain over the keys as logged. It leans on a set staying bad when
  # keys are added to it, so its answer is asked for on its own and ddmin
  # runs when that is not bad.
  def run(keys)
    cand = keys
    found = []
    searching = true
    while searching
      @within = cand
      @jump = []
      @above = []
      found = explain([], [], false, cand)
      if @jump.length > 0
        cand = in_order(@jump, keys)
      else
        searching = false
      end
    end
    found = in_order(found, keys)
    return trim(found) if ask(found) == BS_BAD
    kinds = ddmin(bs_kinds(keys), keys)
    ddmin(bs_of_kinds(keys, kinds), [])
  end
end
