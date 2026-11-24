# Two hooked modules whose class-side modules share a leaf name
# (`ClassMethods`), each extended onto its includer through the hook's
# parameter: each includer answers its own module's accessors, and a class
# including both answers both.
module Taggable
  module ClassMethods
    attr_accessor :tags
  end
  def self.included(klass)
    klass.extend(ClassMethods)
    klass.tags = []
  end
end
module Limited
  module ClassMethods
    attr_accessor :limit
  end
  def self.included(klass)
    klass.extend(ClassMethods)
    klass.limit = 10
  end
end
class Post
  include Taggable
end
class Page
  include Limited
end
class Note
  include Taggable
  include Limited
end
Post.tags << "a"
Note.limit = 3
p Post.tags, Page.limit, Note.tags, Note.limit
p Post.respond_to?(:limit), Page.respond_to?(:tags)
