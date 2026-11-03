# A constant path in a pattern (`in Mod::Klass`) is Mod::Klass === subject,
# as `in Klass` is: the arm matched every subject before.
module M
  class K; end
  class J < K; end
  module Mix; end
  class L; include Mix; end
  P = Struct.new(:x, :y)
  N = 3
  S = "s"
end
module A; class K; end; end

def kind(x)
  case x
  in M::K then :k
  else :no
  end
end
p kind(3), kind([1]), kind(M::K.new), kind(M::J.new), kind(nil)

# poly subject, every arm a path
p [1, "a", M::K.new, M::J.new, nil, :s].map { |v|
  case v
  in M::J then :j
  in M::K then :k
  in ::String then :str
  else :no
  end
}

# same leaf name under two modules
p(case A::K.new; in M::K then :mk; in A::K then :ak; else :no; end)
p(case M::K.new; in A::K then :ak; in M::K then :mk; else :no; end)

# alternation, binding, guard, top-level path, module
p(case 3; in M::K | Integer then :or; else :no; end)
p(case 3; in M::K | String then :or; else :no; end)
p(case 3; in M::K => v then v; else :no; end)
p(case M::J.new; in M::K => v then v.class; else :no; end)
p(case M::K.new; in M::K if true then :g; else :no; end)
p(case M::K.new; in ::M::K then :top; else :no; end)
p(case M::L.new; in M::Mix then :mix; else :no; end)
p(case M::K.new; in M::Mix then :mix; else :no; end)

# inside container patterns
p(case [M::K.new]; in [M::K] then :arr; else :no; end)
p(case [3]; in [M::K] then :arr; else :no; end)
p(case({a: 3}); in {a: M::K} then :h; else :no; end)
p(case({a: M::J.new}); in {a: M::K} then :h; else :no; end)
p(case M::P.new(1, 2); in M::P[x, y] then [x, y]; else :no; end)
p(case 3; in M::P[x, y] then [x, y]; else :no; end)

# a path that names a value: value === subject
p [3, 4, "s", nil].map { |v| case v; in M::N then :n; in M::S then :s; else :no; end }
p(case 3.0; in M::N then :n; else :no; end)
p(case 1.0 / 0; in Float::INFINITY then :inf; else :no; end)

# no arm matches: NoMatchingPatternError
begin
  case 3
  in M::K then p :k
  end
rescue NoMatchingPatternError => e
  p e.class
end

# a constant that names a class tests that class
module M
  class Err < StandardError; end
  AK = K; AE = Err; AI = Integer
end
begin; raise M::Err, "x"; rescue => e; ex = e; end
p [(case M::K.new; in M::AK then :ak; else :no; end),
   (case ex; in M::AE then :ae; else :no; end),
   (case 3; in M::AI then :ai; else :no; end),
   (case "s"; in M::AI then :ai; else :no; end)]

# the NoMatchingPatternError message names the pattern
begin; 3 => M::K; rescue NoMatchingPatternError => e; p e.message; end
begin; 4 => M::N; rescue NoMatchingPatternError => e; p e.message; end
