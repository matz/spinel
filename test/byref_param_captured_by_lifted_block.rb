# A String parameter the body appends to and passes on is the caller's String,
# also when a proc captures it. A block lifted to a proc for a poly receiver's
# `each` captured `io`, and the cell it made was once taken for a reason to
# give the parameter a copy: the answer then depended on which of two passes
# ran first, and the caller's buffer came back empty. A lifted iteration block
# is consumed while the call runs, so its cell changes nothing; a proc that
# can outlive the call (a stored proc, a Thread body) is covered by the other
# probes (#4568, rubys).
class Comment
  def initialize(t) = @t = t
  def t = @t
end
# a user collection with its own yielding `each`: a poly receiver's `each`
# may reach it, so the block below is lifted to a proc
class Relation
  def initialize(items) = @items = items
  def each
    i = 0
    while i < @items.length
      yield @items[i]
      i += 1
    end
  end
end
class Article
  def initialize(n)
    @comments = [Comment.new("a"), Comment.new("b")]
    @n = n
  end
  # an Array on one path, a Relation on the other: boxed
  def comments = @n > 5 ? Relation.new(@comments) : @comments
end
module Views
  def self.comment_into(io, c)
    io << "<li>" << c.t << "</li>"
    nil
  end
  def self.show_into(io, article)
    io << "<h1>title</h1>"
    article.comments.each { |c| Views.comment_into(io, c) }
    io << "<p>end</p>"
    nil
  end
  def self.show(article)
    io = String.new
    Views.show_into(io, article)
    io
  end
end
puts Views.show(Article.new(ARGV.length))
