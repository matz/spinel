# `base.x = v` in a module's `def self.included(base)`, where the hook has
# just given base's singleton class the accessor (or a `def self.x=`): the
# write is spliced into the includer's body as `self.x = v`, which the
# body's own singleton accessor answers. Spliced as a bare `x=(v)`, it was
# a method no class had. A sanitizer's safe-list module seeds each
# includer's allowed tags this way.
module SafeList
  DEFAULT_ALLOWED_TAGS = Set.new(["a", "b"]).freeze
  def self.included(klass)
    class << klass
      attr_accessor :allowed_tags
      def limit=(n); @limit = n * 2; end
      def limit = @limit
    end
    klass.allowed_tags = DEFAULT_ALLOWED_TAGS.dup
    klass.limit = 5
  end
  def tags = self.class.allowed_tags
end
class Sanitizer; end
class SafeListSanitizer < Sanitizer
  include SafeList
end
class Strict
  include SafeList
end
p SafeListSanitizer.allowed_tags.to_a, SafeListSanitizer.new.tags.size, SafeListSanitizer.limit
SafeListSanitizer.allowed_tags << "p"
p SafeListSanitizer.allowed_tags.size, Strict.allowed_tags.size, SafeList::DEFAULT_ALLOWED_TAGS.size, Strict.limit
