BLOXBrowser_FILES = \
packages/blox/browser/Load.st packages/blox/browser/GuiData.st packages/blox/browser/View.st packages/blox/browser/Manager.st packages/blox/browser/RadioForm.st packages/blox/browser/Menu.st packages/blox/browser/ModalDialog.st packages/blox/browser/PList.st packages/blox/browser/PText.st packages/blox/browser/PCode.st packages/blox/browser/ButtonForm.st packages/blox/browser/BrowShell.st packages/blox/browser/BrowserMain.st packages/blox/browser/ClassHierBrow.st packages/blox/browser/ClassBrow.st packages/blox/browser/NamespBrow.st packages/blox/browser/MethSetBrow.st packages/blox/browser/Inspector.st packages/blox/browser/DictInspect.st packages/blox/browser/MethInspect.st packages/blox/browser/StrcInspect.st packages/blox/browser/DebugSupport.st packages/blox/browser/Debugger.st packages/blox/browser/Notifier.st packages/blox/browser/ChangeLog 
$(BLOXBrowser_FILES):
$(srcdir)/packages/blox/browser/stamp-classes: $(BLOXBrowser_FILES)
	touch $(srcdir)/packages/blox/browser/stamp-classes
