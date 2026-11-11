# A reopened Object that defines one of the names a boxed IO, Struct or
# Symbol lacks answers that name only. The other names still raise
# NoMethodError, as in CRuby.
class Object
  def last = :obj_last
end

S = Struct.new(:a, :b)
vals = [$stdout, [1, 2], S.new(1, 2), :sym, nil]
vals.each do |v|
  p((v.last rescue $!.class), (v.length rescue $!.class), (v.join rescue $!.class),
    (v.empty? rescue $!.class), (v.rotate rescue $!.class), (v.at(0) rescue $!.class))
end
# (an IO is Enumerable: $stdout raises IOError for these, left out here)
vals.drop(1).each do |v|
  p((v.member?(1) rescue $!.class), (v.include?(1) rescue $!.class), (v.count rescue $!.class))
end
