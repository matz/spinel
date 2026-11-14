# spinel: not-cruby -- the .expected is spinel's own --dump-order output
# --dump-order=all prints the operand-order record: one sorted line per
# operand list, with where each operand is evaluated (AHEAD: it runs code
# and goes in its place ahead of the consumer; HOLD: a read a later
# operand may change, taken in its place; INLINE: read at the consumer),
# whether it runs code and whether code runs after it (across), or `none`
# for a list with nothing to bind (an empty one too). The shapes are the
# minimal programs of the operand-order groups, and reads beside a part
# whose implicit to_s, to_proc, to_int (an Integer's operator over an
# object) or **-splat may run a method of the program's.
require "tmpdir"

PROGRAMS = [
  ["g10_interp_matz2.rb", <<~'RUBY'],
    s = +"ab"
    p "#{s.upcase!.to_s}-#{s << "!"}"
  RUBY
  ["g11_aset_value.rb", <<~'RUBY'],
    def m = (@s << "z"; +"qrs")
    @s = +"ab"
    p(m[2] = @s)
  RUBY
  ["g1_center_proc_mut.rb", <<~'RUBY'],
    s = +"ab"
    g = proc { s << "z"; "q" }
    p s.center(2, g.call)
  RUBY
  ["g1_center_reas.rb", <<~'RUBY'],
    def run
      s = +"ab"
      p s.center((s = +"WXYZ"; 2), +"q")
    end
    run
  RUBY
  ["g2_blockpass.rb", <<~'RUBY'],
    def fb(a, &b) = b.call(a)
    s = +"ab"
    p fb(s, &(s = +"wxyz"; :upcase))
  RUBY
  ["g3_match_order.rb", <<~'RUBY'],
    $t = []
    p(($t << 1; +"q") =~ ($t << 2; /b/))
    p $t
  RUBY
  ["g4_mutator_recv.rb", <<~'RUBY'],
    def run
      s = +"ab"
      g = proc { s = +"WXYZ"; 2 }
      p s.insert(g.call, "Q")
    end
    run
  RUBY
  ["g5_user_recv.rb", <<~'RUBY'],
    class K
      def initialize(v) = (@v = v)
      def k(x) = "#{@v}|#{x}"
    end
    o = K.new(3)
    p o.k((o = K.new(8); "q"))
  RUBY
  ["g6_opasgn_answer.rb", <<~'RUBY'],
    a = [1, 2, 3]
    p(a[2] += (a = [7]; 2))
  RUBY
  ["g7_blockpass_nil.rb", <<~'RUBY'],
    def fb(a) = [a, block_given?]
    p fb(1, &(puts "side"; nil))
  RUBY
  ["g7_fetch_default.rb", <<~'RUBY'],
    a = [1, 2, 3]
    p a.fetch(2, (puts "default ran"; 0))
  RUBY
  ["g8_push_interleave.rb", <<~'RUBY'],
    a = [1, 2, 3]
    p a.push(2, (a << 9; 2))
  RUBY
  ["g9_splat_late.rb", <<~'RUBY'],
    def f(*r) = r
    @a = [1, 2, 3]
    p f(*@a, (@a << 9; 2))
  RUBY
  ["b1.rb", <<~'RUBY'],
    class T
      def initialize(c) = @c = c
      def to_s; @c.bump; "t"; end
    end
    class C
      attr_reader :a
      def initialize; @a = 1; end
      def bump; @a += 1; end
      def f(*xs) = xs
      def g1(t) = f(@a, "#{t}")
      def g2(t) = [@a, "#{t}"]
    end
    c = C.new
    t = T.new(c)
    p c.g1(t)
    p c.g2(t)
  RUBY
  ["b4.rb", <<~'RUBY'],
    class C
      def initialize; @a = [1]; end
      def bump; @a = [2]; end
      def f(*xs, **kw, &b) = [xs, kw, b ? b.call : nil]
      def to_s; bump; "s"; end
      def to_hash; bump; {q: 1}; end
      def to_proc; bump; proc { 0 }; end
      def g1 = f(@a, "#{self}")
      def g2 = f(@a, **self)
      def g3 = f(@a, &self)
      def g4 = [@a, "#{self}"]
      def g5 = {k: @a, j: "#{self}"}
    end
    p C.new.g1
    p C.new.g2
    p C.new.g3
    p C.new.g4
    p C.new.g5
  RUBY
  ["b5.rb", <<~'RUBY'],
    class T
      def initialize(pr) = @pr = pr
      def to_s; @pr.call; "t"; end
    end
    def m
      x = 1
      pr = -> { x = 2 }
      t = T.new(pr)
      [x, "#{t}"]
    end
    p m
    def m2
      x = [1]
      pr = -> { x = [2] }
      t = T.new(pr)
      [x, "#{t}", 3]
    end
    p m2
    def m3
      x = 1
      y = 5
      pr = -> { x = 2 }
      [y, pr.call, x]
    end
    p m3
    def m4(c)
      x = 1
      [c ? x : 0, (x = 2)]
    end
    p m4(true)
    def m5
      a = nil
      b = 1
      a&.foo(b, (b = 2))
      [b]
    end
    p m5
  RUBY
  ["c1_shift_to_int.rb", <<~'RUBY'],
    class O
      def initialize(k) = @k = k
      def to_int; @k.bump; 2; end
    end
    class K
      def initialize; @x = 1; end
      def bump; @x = 9; end
      def run(o) = [@x, 1 << o]
      def plain(n) = [@x, 1 << n]
    end
    k = K.new
    p k.run(O.new(k))
    p k.plain(2)
  RUBY
  ["c2_rescue_cvar.rb", <<~'RUBY'],
    class C
      @@x = 1
      def self.m(d) = [@@x, (begin; 1 / d; rescue => @@x; end)]
      def self.f = [@@x, (for @@x in [5]; end)]
    end
    p C.m(0)
    p C.f
  RUBY
  ["c3_reader_literal_recv.rb", <<~'RUBY'],
    class K
      attr_reader :w
      def initialize = (@w = 1)
    end
    class L
      attr_reader :w
      def initialize = (@w = 2)
    end
    class Integer
      def w = (C.bump; 0)
    end
    class C
      @@x = 1
      def self.bump = (@@x = 9)
      def self.m(o) = [@@x, o.w]
      def self.n(o) = [@@x, o.w]
    end
    p C.m(K.new)
    p C.m(3)
    p C.n(K.new)
    p C.n(L.new)
  RUBY
  ["c4_local_write_chain.rb", <<~'RUBY'],
    def f(a) = a
    def g(a, b) = [a, b]
    x = 0
    a = 1
    p f(f(f(x = 1)))
    p g(a, g(a, g(a, (a = 2))))
    p [a, x]
  RUBY
  ["c4_ivar_writes.rb", <<~'RUBY'],
    class K
      def initialize = (@a = 1; @b = 2; @c = 3)
      def set_a = (@a = 9)
      def three = (@c = 1; @c = 2; @c = 3)
      def run = [@a, @b, three, set_a, (@b = 5)]
    end
    p K.new.run
  RUBY
  ["c5_empty_lists.rb", <<~'RUBY'],
    def foo = 1
    a = []
    h = {}
    p a, h, foo
  RUBY
]

dir = File.join(Dir.tmpdir, "spinel_order_dump_#{Process.pid}")
Dir.mkdir(dir)
PROGRAMS.each do |name, src|
  path = File.join(dir, name)
  File.write(path, src)
  puts "== #{name}"
  puts IO.popen(["bin/spinel", "--dump-order=all", path], &:read)
  File.delete(path)
end
Dir.rmdir(dir)
