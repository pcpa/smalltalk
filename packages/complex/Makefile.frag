Complex_FILES = \
packages/complex/Complex.st packages/complex/ChangeLog packages/complex/complextests.st 
$(Complex_FILES):
$(srcdir)/packages/complex/stamp-classes: $(Complex_FILES)
	touch $(srcdir)/packages/complex/stamp-classes
