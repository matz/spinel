# A loop nested in another leaves its signal poll to the enclosing loop
# (#9222), and a builtin iterator's own loop (map, sum, select, ...) takes
# none, but not when a pass walks what a String holds: a String's length
# counts the characters of one not known to be ASCII, and count, include?
# and their like read every character, so a single walk of such passes could
# hold a trap for seconds. Such a loop or step polls at each pass, so the
# trap can run part way through a walk, where it finds the walk's pass
# (@cur); each walk sets @cur to -1 when it ends, which is all a trap finds
# with the poll left to the enclosing loop or to the iterator's end. A case
# tries a few times, as a trap may also land between two walks.
$stdout.sync = true

def kick
  Process.spawn("sh", "-c", "sleep 0.05; kill -USR1 #{Process.pid}")
end

class W
  attr_accessor :s, :cur
  def initialize; @s = 0; @cur = -1; end

  # a while in a while
  def lens(words, reps)
    r = 0
    while r < reps
      j = 0
      while j < words.length
        @s += words[j].length
        @cur = j
        j += 1
      end
      @cur = -1
      r += 1
    end
  end

  # ...whose pass counts or searches the String instead
  def counts(words, reps)
    reps.times do
      j = 0
      while j < words.length
        @s += words[j].count("x") + (words[j].include?("x") ? 1 : 0)
        @cur = j
        j += 1
      end
      @cur = -1
    end
  end

  # ...over an Array that holds an Integer too, so each element is boxed
  def boxed(words, reps)
    xs = (@mixed ||= words + [7])
    reps.times do
      j = 0
      while j < xs.length
        x = xs[j]
        @s += 1 if x.is_a?(String) && x.length > 0
        @cur = j
        j += 1
      end
      @cur = -1
    end
  end

  # an each_with_index in a times
  def each_lens(words, reps)
    reps.times do
      words.each_with_index do |w, j|
        @s += w.length
        @cur = j
      end
      @cur = -1
    end
  end

  # a while whose yield splices in a block that counts characters
  def each_word(words, reps)
    r = 0
    while r < reps
      j = 0
      while j < words.length
        yield words[j]
        @cur = j
        j += 1
      end
      @cur = -1
      r += 1
    end
  end

  def yield_lens(words, reps)
    each_word(words, reps) { |w| @s += w.length }
  end

  # ...and one whose spliced block yields in turn
  def each_twice(words, reps)
    reps.times { words.each_with_index { |w, j| yield w; @cur = j }; @cur = -1 }
  end

  def two_level(words, reps)
    each_twice(words, reps) { |w| @s += w.length }
  end

  # a block called by name, which is spliced as a yield is
  def each_call(words, reps, &b)
    reps.times { words.each_with_index { |w, j| b.call(w); @cur = j }; @cur = -1 }
  end

  def call_lens(words, reps)
    each_call(words, reps) { |w| @s += w.length }
  end

  # builtin iterators, nested and not
  def map_lens(words, reps)
    reps.times { j = -1; @s += words.map { |w| j += 1; @cur = j; w.length }.size; @cur = -1 }
  end

  def sum_lens(words, reps)
    reps.times { j = -1; @s += words.sum { |w| j += 1; @cur = j; w.length }; @cur = -1 }
  end

  def select_lens(words, reps)
    reps.times { j = -1; @s += words.select { |w| j += 1; @cur = j; w.length > 202 }.size; @cur = -1 }
  end

  def inject_lens(words, reps)
    reps.times { j = -1; @s += words.inject(0) { |t, w| j += 1; @cur = j; t + w.length }; @cur = -1 }
  end

  def sort_by_lens(words, reps)
    reps.times { j = -1; @s += words.sort_by { |w| j += 1; @cur = j; w.length }.size; @cur = -1 }
  end

  # a step with a `next`, which runs in a frame of its own
  def next_map(words, reps)
    reps.times { j = -1; @s += words.map { |w| j += 1; @cur = j; next 0 if w.empty?; w.length }.size; @cur = -1 }
  end

  # each_char, whose pass reads a long String
  def chars_lens(words, reps)
    big = words.first(10).join
    reps.times { j = -1; ("x" * N).each_char { |_ch| j += 1; @cur = j; @s += big.length }; @cur = -1 }
  end

  # a Hash's map (the Hash built once, before any case: under GC stress
  # its build outlasts the trap's delay)
  def hash_map_lens(_words, reps)
    h = $hw
    reps.times { j = -1; @s += h.map { |_k, w| j += 1; @cur = j; w.length }.size; @cur = -1 }
  end

  def top_map(words, _reps)
    j = -1
    @s += words.map { |w| j += 1; @cur = j; w.length }.size
    @cur = -1
  end
end

# a pass that does not read what the String holds (its bytesize is stored)
# takes no poll: the iterator yields when it returns
def tight_sum(words) = words.map { |w| w.bytesize }.sum

N = 20_000
words = Array.new(N) { |i| "é" * (200 + i % 7) }
# Does a trap, in one of a few tries, find walk m in progress?
def lands_inside(m, arg)
  5.times do
    o = W.new
    $at = nil
    Signal.trap(:USR1) { $at = o.cur }
    k = kick
    o.send(m, arg, 3) while $at.nil?
    Process.wait(k)
    return true if $at >= 0
  end
  false
end

$hw = words.each_with_index.to_h { |w, i| [i, w] }
%i[lens counts boxed each_lens yield_lens two_level call_lens map_lens sum_lens select_lens
   inject_lens sort_by_lens next_map chars_lens hash_map_lens top_map].each do |m|
  p [m, lands_inside(m, words)]
end
p tight_sum(words)
