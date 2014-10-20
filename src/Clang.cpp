/* This file is part of RTags.

RTags is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTags is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#include "Clang.h"
#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS
#include <clang/Basic/Version.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclVisitor.h>
#include <clang/AST/StmtVisitor.h>
#include <clang/AST/RecursiveASTVisitor.h>

static inline std::string getDeclName(const clang::Decl *D) {
    if (clang::isa<clang::NamedDecl>(D))
        return clang::cast<clang::NamedDecl>(D)->getQualifiedNameAsString();
    return std::string();
}

static inline std::string getDeclLoc(const clang::Decl *D, const clang::SourceManager* sm)
{
    return D->getLocStart().printToString(*sm);
}

static inline const clang::Decl* definition(const clang::Decl* decl)
{
    if (decl) {
        if (clang::isa<clang::VarDecl>(decl)) {
            const clang::VarDecl* vd = clang::cast<clang::VarDecl>(decl);
            const clang::Decl* def = vd->getDefinition();
            if (def)
                decl = def;
        }
    }
    return decl;
}

static inline std::string typeLocation(const clang::Type* t, const clang::SourceManager* sm)
{
    if (const clang::RecordType* ct = t->getAsStructureType()) {
        return ct->getDecl()->getLocation().printToString(*sm);
    } else if (const clang::RecordType* ct = t->getAsUnionType()) {
        return ct->getDecl()->getLocation().printToString(*sm);
    } else if (const clang::CXXRecordDecl* cxx = t->getAsCXXRecordDecl()) {
        return cxx->getLocation().printToString(*sm);
    } else if (const clang::CXXRecordDecl* cxx = t->getPointeeCXXRecordDecl()) {
        return cxx->getLocation().printToString(*sm);
    } else if (t->isPointerType()) {
        const clang::SplitQualType st = t->getPointeeType().getSplitDesugaredType();
        if (st.Ty && st.Ty != t)
            return typeLocation(st.Ty, sm);
    } else if (const clang::TemplateSpecializationType* tt = t->getAs<const clang::TemplateSpecializationType>()) {
        if (clang::TemplateDecl* d = tt->getTemplateName().getAsTemplateDecl()) {
            return d->getLocation().printToString(*sm);
        }
    } else if (t->isLValueReferenceType() || t->isRValueReferenceType()) {
        const clang::SplitQualType st = t->getCanonicalTypeUnqualified().getNonReferenceType().split();
        if (st.Ty && st.Ty != t)
            return typeLocation(st.Ty, sm);
    } else {
        // try the canonical type if it's different from ourselves
        const clang::CanQualType ct = t->getCanonicalTypeUnqualified();
        const clang::SplitQualType st = ct.split();
        if (st.Ty && st.Ty != t)
            return typeLocation(st.Ty, sm);
    }
    //error() << "unhandled type" << t->getTypeClassName();
    return std::string();
}

class RTagsCompilationDatabase : public clang::tooling::CompilationDatabase
{
public:
    RTagsCompilationDatabase(const Source &source, const String &unsaved = String())
        : mSource(source), mUnsaved(unsaved)
    {
        mCommand.Directory = source.pwd;
        const unsigned int commandLineFlags = (Source::FilterBlacklist
                                               | Source::IncludeDefines
                                               | Source::IncludeIncludepaths
                                               | Source::IncludeSourceFile
                                               | Source::IncludeLibClangOptions);
        const List<String> args = source.toCommandLine(commandLineFlags);
        mCommand.CommandLine.resize(args.size());
        int i = 0;

        for (const auto &str : args) {
            mCommand.CommandLine[i++] = str;
        }
        if (!unsaved.isEmpty()) {
            mCommand.MappedSources.push_back(std::make_pair(source.sourceFile(), unsaved));
        }
    }

    virtual std::vector<clang::tooling::CompileCommand> getCompileCommands(llvm::StringRef file) const
    {
        Path path(file.data(), file.size());
        if (path.isSameFile(mSource.sourceFile()))
            return getAllCompileCommands();
        return std::vector<clang::tooling::CompileCommand>();
    }

    virtual std::vector<std::string> getAllFiles() const
    {
        return std::vector<std::string>(1, mSource.sourceFile());
    }

    virtual std::vector<clang::tooling::CompileCommand> getAllCompileCommands() const
    {
        return std::vector<clang::tooling::CompileCommand>(1, mCommand);
    }
private:
    clang::tooling::CompileCommand mCommand;
    const Source mSource;
    const String mUnsaved;
};

class RTagsDeclVisitor : public clang::ConstDeclVisitor<RTagsDeclVisitor>,
                         public clang::ConstStmtVisitor<RTagsDeclVisitor>
{
public:
    RTagsDeclVisitor()
        : mSourceManager(0)
    {
    }

    void setSourceManager(const clang::SourceManager* sm)
    {
        mSourceManager = sm;
    }

    void visitDecl(const clang::Decl* d)
    {
        if (d)
            clang::ConstDeclVisitor<RTagsDeclVisitor>::Visit(d);
    }

    void visitStmt(const clang::Stmt* s)
    {
        if (!s)
            return;
        //error() << "stmt" << s->getStmtClassName();
        if (const clang::DeclStmt *DS = clang::dyn_cast<clang::DeclStmt>(s)) {
            VisitDeclStmt(DS);
            return;
        }
        clang::ConstStmtVisitor<RTagsDeclVisitor>::Visit(s);
        for (clang::Stmt::const_child_range CI = s->children(); CI; ++CI) {
            // Stmt::const_child_range Next = CI;
            // ++Next;
            // if (!Next)
            //     lastChild();
            visitStmt(*CI);
        }
    }

    void VisitLabelDecl(const clang::LabelDecl *D)
    {
    }

    void VisitTypedefDecl(const clang::TypedefDecl *D)
    {
    }

    void VisitEnumDecl(const clang::EnumDecl *D)
    {
    }

    void VisitRecordDecl(const clang::RecordDecl *D)
    {
    }

    void VisitEnumConstantDecl(const clang::EnumConstantDecl *D)
    {
        if (const clang::Expr *Init = D->getInitExpr()) {
            visitStmt(Init);
        }
    }

    void VisitIndirectFieldDecl(const clang::IndirectFieldDecl *D)
    {
    }

    void VisitFunctionDecl(const clang::FunctionDecl *D)
    {
        error() << "func decl" << getDeclName(D) << getDeclLoc(D, mSourceManager);
        const clang::CXXConstructorDecl *C = clang::dyn_cast<clang::CXXConstructorDecl>(D);
        if (C) {
            for (clang::CXXConstructorDecl::init_const_iterator I = C->init_begin(), E = C->init_end(); I != E; ++I) {
                // if (I + 1 == E)
                //     lastChild();
                const clang::CXXCtorInitializer* init = *I;
                if (init->isAnyMemberInitializer()) {
                    visitDecl(init->getAnyMember());
                } else if (init->isBaseInitializer()) {
#warning do stuff with getBaseClass()
                } else if (init->isDelegatingInitializer()) {
#warning do stuff with getTypeSourceInfo()
                }
                visitStmt(init->getInit());
            }
        }
        visitStmt(D->getBody());
    }

    void VisitFieldDecl(const clang::FieldDecl *D)
    {
        if (clang::Expr *Init = D->getInClassInitializer()) {
            visitStmt(Init);
        }
    }

    void VisitVarDecl(const clang::VarDecl *D)
    {
        error() << "got var" << getDeclName(D) << getDeclLoc(D, mSourceManager);
        const clang::QualType t = D->getType();
        if (!t.isNull()) {
            //const clang::SplitQualType st = t.getSplitDesugaredType();
            const clang::SplitQualType st = t.getSplitDesugaredType();
            error() << "  " << clang::QualType::getAsString(st);
            if (st.Ty) {
                error() << "    defined at" << typeLocation(st.Ty, mSourceManager);
            }
        }
        if (D->hasInit()) {
            visitStmt(D->getInit());
        }
    }

    void VisitFileScopeAsmDecl(const clang::FileScopeAsmDecl *D)
    {
    }

    void VisitImportDecl(const clang::ImportDecl *D)
    {
    }

    void VisitNamespaceDecl(const clang::NamespaceDecl *D)
    {
    }

    void VisitUsingDirectiveDecl(const clang::UsingDirectiveDecl *D)
    {
    }

    void VisitNamespaceAliasDecl(const clang::NamespaceAliasDecl *D)
    {
    }

    void VisitTypeAliasDecl(const clang::TypeAliasDecl *D)
    {
    }

    void VisitTypeAliasTemplateDecl(const clang::TypeAliasTemplateDecl *D)
    {
    }

    void VisitCXXRecordDecl(const clang::CXXRecordDecl *D)
    {
        error() << "got cxx record" << getDeclName(D) << getDeclLoc(D, mSourceManager);
    }

    void VisitStaticAssertDecl(const clang::StaticAssertDecl *D)
    {
        visitStmt(D->getAssertExpr());
        visitStmt(D->getMessage());
    }

    void VisitFunctionTemplateDecl(const clang::FunctionTemplateDecl *D)
    {
    }

    void VisitClassTemplateDecl(const clang::ClassTemplateDecl *D)
    {
    }

    void VisitClassTemplateSpecializationDecl(const clang::ClassTemplateSpecializationDecl *D)
    {
    }

    void VisitClassTemplatePartialSpecializationDecl(const clang::ClassTemplatePartialSpecializationDecl *D)
    {
    }

    void VisitClassScopeFunctionSpecializationDecl(const clang::ClassScopeFunctionSpecializationDecl *D)
    {
    }

    void VisitVarTemplateDecl(const clang::VarTemplateDecl *D)
    {
    }

    void VisitVarTemplateSpecializationDecl(const clang::VarTemplateSpecializationDecl *D)
    {
    }

    void VisitVarTemplatePartialSpecializationDecl(const clang::VarTemplatePartialSpecializationDecl *D)
    {
    }

    void VisitTemplateTypeParmDecl(const clang::TemplateTypeParmDecl *D)
    {
    }

    void VisitNonTypeTemplateParmDecl(const clang::NonTypeTemplateParmDecl *D)
    {
    }

    void VisitTemplateTemplateParmDecl(const clang::TemplateTemplateParmDecl *D)
    {
    }

    void VisitUsingDecl(const clang::UsingDecl *D)
    {
    }

    void VisitUnresolvedUsingTypenameDecl(const clang::UnresolvedUsingTypenameDecl *D)
    {
    }

    void VisitUnresolvedUsingValueDecl(const clang::UnresolvedUsingValueDecl *D)
    {
    }

    void VisitUsingShadowDecl(const clang::UsingShadowDecl *D)
    {
    }

    void VisitLinkageSpecDecl(const clang::LinkageSpecDecl *D)
    {
    }

    void VisitAccessSpecDecl(const clang::AccessSpecDecl *D)
    {
    }

    void VisitFriendDecl(const clang::FriendDecl *D)
    {
    }

    void VisitDeclStmt(const clang::DeclStmt *Node)
    {
        for (clang::DeclStmt::const_decl_iterator I = Node->decl_begin(), E = Node->decl_end(); I != E; ++I) {
            // if (I + 1 == E)
            //     lastChild();
            visitDecl(*I);
        }
    }

    void VisitAttributedStmt(const clang::AttributedStmt *Node)
    {
    }

    void VisitLabelStmt(const clang::LabelStmt *Node)
    {
    }

    void VisitGotoStmt(const clang::GotoStmt *Node)
    {
    }

    void VisitCXXCatchStmt(const clang::CXXCatchStmt *Node)
    {
    }

    void VisitCastExpr(const clang::CastExpr *Node)
    {
        //error() << "cast to" << Node->getCastKindName();
    }

    void VisitDeclRefExpr(const clang::DeclRefExpr *Node)
    {
        error() << "decl ref" << Node->getLocation().printToString(*mSourceManager);
        const clang::Decl* decl = definition(Node->getDecl());
        if (decl) {
            assert(decl);
            error() << " -> " << getDeclName(decl) << getDeclLoc(decl, mSourceManager) << decl->getDeclKindName();
        }
    }

    void VisitPredefinedExpr(const clang::PredefinedExpr *Node)
    {
    }

    void VisitCharacterLiteral(const clang::CharacterLiteral *Node)
    {
    }

    void VisitIntegerLiteral(const clang::IntegerLiteral *Node)
    {
    }

    void VisitFloatingLiteral(const clang::FloatingLiteral *Node)
    {
    }

    void VisitStringLiteral(const clang::StringLiteral *Str)
    {
    }

    void VisitInitListExpr(const clang::InitListExpr *ILE)
    {
    }

    void VisitUnaryOperator(const clang::UnaryOperator *Node)
    {
    }

    void VisitUnaryExprOrTypeTraitExpr(const clang::UnaryExprOrTypeTraitExpr *Node)
    {
    }

    void VisitMemberExpr(const clang::MemberExpr *Node)
    {
        error() << "member expr" << Node->getExprLoc().printToString(*mSourceManager);
        const clang::Decl* decl = definition(Node->getMemberDecl());
        if (decl)
            error() << " -> " << getDeclName(decl) << getDeclLoc(decl, mSourceManager) << decl->getDeclKindName();
    }

    void VisitExtVectorElementExpr(const clang::ExtVectorElementExpr *Node)
    {
    }

    void VisitBinaryOperator(const clang::BinaryOperator *Node)
    {
    }

    void VisitCompoundAssignOperator(const clang::CompoundAssignOperator *Node)
    {
    }

    void VisitAddrLabelExpr(const clang::AddrLabelExpr *Node)
    {
    }

    void VisitBlockExpr(const clang::BlockExpr *Node)
    {
    }

    void VisitCallExpr(const clang::CallExpr *Node)
    {
        error() << "call expr";
        const clang::Decl* decl = Node->getCalleeDecl();
        if (decl)
            error() << " -> " << getDeclName(decl) << getDeclLoc(decl, mSourceManager) << decl->getDeclKindName();
        const unsigned int n = Node->getNumArgs();
        const clang::Expr* const* a = Node->getArgs();
        for (unsigned int i = 0; i < n; ++i) {
            visitStmt(a[i]);
        }
    }

    void VisitOpaqueValueExpr(const clang::OpaqueValueExpr *Node)
    {
    }

    // C++
    void VisitCXXNamedCastExpr(const clang::CXXNamedCastExpr *Node)
    {
    }

    void VisitCXXBoolLiteralExpr(const clang::CXXBoolLiteralExpr *Node)
    {
    }

    void VisitCXXThisExpr(const clang::CXXThisExpr *Node)
    {
    }

    void VisitCXXFunctionalCastExpr(const clang::CXXFunctionalCastExpr *Node)
    {
    }

    void VisitCXXConstructExpr(const clang::CXXConstructExpr *Node)
    {
    }

    void VisitCXXBindTemporaryExpr(const clang::CXXBindTemporaryExpr *Node)
    {
    }

    void VisitMaterializeTemporaryExpr(const clang::MaterializeTemporaryExpr *Node)
    {
    }

    void VisitExprWithCleanups(const clang::ExprWithCleanups *Node)
    {
    }

    void VisitUnresolvedLookupExpr(const clang::UnresolvedLookupExpr *Node)
    {
    }

    void dumpCXXTemporary(const clang::CXXTemporary *Temporary)
    {
    }

    void VisitLambdaExpr(const clang::LambdaExpr *Node)
    {
    }

private:
    const clang::SourceManager* mSourceManager;
};

class RTagsASTConsumer : public clang::ASTConsumer, public clang::RecursiveASTVisitor<RTagsASTConsumer>
{
    typedef clang::RecursiveASTVisitor<RTagsASTConsumer> base;
public:
    RTagsASTConsumer(Clang *clang)
        : mClang(clang), mAborted(false)
    {}

    void HandleTranslationUnit(clang::ASTContext &Context) override {
        clang::TranslationUnitDecl *D = Context.getTranslationUnitDecl();
        mSourceManager = &Context.getSourceManager();
        mDeclVisitor.setSourceManager(mSourceManager);
        TraverseDecl(D);
    }

    bool shouldWalkTypesOfTypeLocs() const { return true; } // ### ???

    bool TraverseDecl(clang::Decl *d) {
        if (mAborted)
            return true;
        if (d) {
            mDeclVisitor.visitDecl(d);
        }
        return base::TraverseDecl(d);
    }

private:
    // void print(clang::Decl *D) {
        // if (DumpLookups) {
        //     if (clang::DeclContext *DC = clang::dyn_cast<clang::DeclContext>(D)) {
        //         if (DC == DC->getPrimaryContext())
        //             DC->dumpLookups(Out, Dump);
        //         else
        //             Out << "Lookup map is in primary DeclContext "
        //                 << DC->getPrimaryContext() << "\n";
        //     } else
        //         Out << "Not a DeclContext\n";
        // } else if (Dump)
        //     D->dump(Out);
        // else
        //     D->print(Out, /*Indentation=*/0, /*PrintInstantiation=*/true);
    // }
    RTagsDeclVisitor mDeclVisitor;
    Clang *mClang;
    bool mAborted;
    const clang::SourceManager* mSourceManager;
};

class RTagsPPCallbacks : public clang::PPCallbacks
{
public:
    RTagsPPCallbacks(Clang *clang, const clang::SourceManager& sm)
        : mClang(clang), mSourceManager(sm)
    {
    }

    virtual void FileChanged(clang::SourceLocation Loc,
                             FileChangeReason Reason,
                             clang::SrcMgr::CharacteristicKind FileType,
                             clang::FileID PrevFID)
    {
        const clang::StringRef& fn = mSourceManager.getFilename(Loc);
        if (fn.empty())
            mCurrentFile.clear();
        else
            mCurrentFile = Path(fn.data(), fn.size());
    }

    virtual void InclusionDirective(clang::SourceLocation HashLoc,
                                    const clang::Token& IncludeTok,
                                    clang::StringRef FileName,
                                    bool IsAngled,
                                    clang::CharSourceRange FilenameRange,
                                    const clang::FileEntry* File,
                                    clang::StringRef SearchPath,
                                    clang::StringRef RelativePath,
                                    const clang::Module* Imported)
    {
        //error() << "included" << File->getName();
    }

private:
    Clang* mClang;
    Path mCurrentFile;
    const clang::SourceManager& mSourceManager;
};

class RTagsFrontendAction : public clang::ASTFrontendAction
{
public:
    RTagsFrontendAction(Clang *clang)
        : mClang(clang)
    {}
#if CLANG_VERSION_MAJOR > 3 || (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 6)
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, clang::StringRef InFile) override
    {
        return std::unique_ptr<clang::ASTConsumer>(new RTagsASTConsumer(mClang));
    }
#else
    clang::ASTConsumer *CreateASTConsumer(clang::CompilerInstance &CI, clang::StringRef InFile) override
    {
        return new RTagsASTConsumer(mClang);
    }
#endif
    void ExecuteAction() override
    {
        clang::Preprocessor& pre = getCompilerInstance().getPreprocessor();
        const clang::SourceManager& manager = pre.getSourceManager();
#if CLANG_VERSION_MAJOR > 3 || (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 6)
        pre.addPPCallbacks(std::unique_ptr<RTagsPPCallbacks>(new RTagsPPCallbacks(mClang, manager)));
#else
        pre.addPPCallbacks(new RTagsPPCallbacks(mClang, manager));
#endif
        clang::ASTFrontendAction::ExecuteAction();
    }
private:
    Clang *mClang;
};

class RTagsFrontendActionFactory : public clang::tooling::FrontendActionFactory
{
public:
    RTagsFrontendActionFactory(Clang *clang)
        : mClang(clang)
    {}
    virtual clang::FrontendAction *create()
    {
        return new RTagsFrontendAction(mClang);
    }
private:
    Clang *mClang;
};

bool Clang::index(const Source &source, const String &unsaved)
{
    RTagsCompilationDatabase compilationDatabase(source);
    clang::tooling::ClangTool tool(compilationDatabase, compilationDatabase.getAllFiles());
    RTagsFrontendActionFactory factory(this);
    tool.run(&factory);
    return true;
}
