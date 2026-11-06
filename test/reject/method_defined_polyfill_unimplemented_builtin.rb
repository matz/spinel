# spinel: reject-subclass: Proc#ruby2_keywords is not supported
# CRuby's Proc has ruby2_keywords, so method_defined? is true and the
# polyfill's def does not run: the call reaches the builtin, which Spinel
# does not implement, and is refused rather than running the polyfill.
class Proc
  unless method_defined?(:ruby2_keywords)
    def ruby2_keywords = self
  end
end
p proc {}.ruby2_keywords.class
