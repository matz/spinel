# A file a class body requires, which a later top-level statement requires
# again: it loads at the class body's require, ahead of the include, and the
# later require loads nothing.
class Article
  require_relative "class_body_require_again_lib"
  include Article::Executable

  def title_text
    "t"
  end
end
require_relative "class_body_require_again_lib"

p Article.new.title_given?
p Article.new.via_send
p Article.ancestors.include?(Article::Executable)
