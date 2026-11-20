# A reopened Rational's #to_json inside a container, as CRuby's json calls
# it. A Rational or Complex reopening is moved into Object's method of the
# name behind `self.is_a?(Rational)`; the json hook calls that method for a
# boxed Rational, and a Complex the reopening does not guard keeps
# Object#to_json's answer, its #to_s as a JSON string.
require "json"

class Rational
  def to_json(*) = '"Q:' + to_s + '"'
end

p({ "q" => 1r, "c" => 1i, "n" => 2 }.to_json)
p [2r, 3i, 1.5, (1/3r)].to_json
p JSON.generate([1r])
