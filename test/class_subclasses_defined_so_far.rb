# Class#subclasses lists the subclasses defined so far, as CRuby's does: a
# class is a subclass from the moment its header runs, not from the start of
# the program. A library that walks a class's descendants as classes load
# (Rails' DescendantsTracker, run from class bodies) sees only those loaded.
class Base
  def self.kids = subclasses.map { |k| k.to_s }.sort
end
p Base.subclasses, Base.kids
class Kid1 < Base; end
p Base.kids
class Kid2 < Base
  p Base.kids   # Kid2 is a subclass as soon as its header has run
end
def later = Base.subclasses.size
p later
class Kid3 < Base; end
p later
k = Base
p k.subclasses.map { |x| x.to_s }.sort
class GKid < Kid1; end
p Kid1.subclasses.map { |x| x.to_s }, Kid2.subclasses, Base.kids
