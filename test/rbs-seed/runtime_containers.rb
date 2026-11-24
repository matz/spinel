# spinel: rbs-seed-run
# Runtime values retain their identity and payload through seeded containers.
class RuntimeContainerValues
  #: (Array[Time]) -> Array[Time]
  def times(xs) = xs
  #: (Array[Regexp]) -> Array[Regexp]
  def regexps(xs) = xs
  #: (Array[MatchData]) -> Array[MatchData]
  def matches(xs) = xs
  #: (Array[Complex]) -> Array[Complex]
  def complexes(xs) = xs
  #: (Array[Rational]) -> Array[Rational]
  def rationals(xs) = xs
  #: (Array[Range]) -> Array[Range]
  def ranges(xs) = xs
  #: (Array[IO]) -> Array[IO]
  def ios(xs) = xs
  #: (Hash[Symbol, Time]) -> Hash[Symbol, Time]
  def stamps(h) = h
  #: (Hash[String, Regexp]) -> Hash[String, Regexp]
  def patterns(h) = h
  #: (Hash[Time, String]) -> Hash[Time, String]
  def keyed(h) = h
  #: (Array[File]) -> Array[File]
  def files(xs) = xs
end
values = RuntimeContainerValues.new
p values.times([Time.at(123)])[0].to_i
p values.regexps([/ab/])[0].source
p values.matches([/a(b)/.match("ab")])[0].to_s
p values.complexes([Complex(2, 3)])[0].imaginary
p values.rationals([Rational(3, 2)])[0].numerator
p values.ranges([(1..3)])[0].last
p values.ios([$stdout])[0].closed?
p values.stamps({now: Time.at(456)})[:now].to_i
p values.patterns({"x" => /xy/})["x"].source
p values.keyed({Time.at(12) => "twelve"}).values[0]
File.open(__FILE__) do |f|
  p values.files([f])[0].equal?(f)
end
