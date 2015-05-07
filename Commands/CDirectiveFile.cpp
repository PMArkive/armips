#include "stdafx.h"
#include "Commands/CDirectiveFile.h"
#include "Core/Common.h"
#include "Util/FileClasses.h"
#include "Core/FileManager.h"

//
// CDirectiveFile
//

CDirectiveFile::CDirectiveFile()
{
	type = Type::Invalid;
	file = nullptr;
}

void CDirectiveFile::initOpen(const std::wstring& fileName, u64 memory)
{
	type = Type::Open;
	std::wstring fullName = getFullPathName(fileName);

	if (fileExists(fullName) == false)
	{
		Logger::printError(Logger::Error,L"File %s not found",fullName);
		return;
	}

	file = new GenericAssemblerFile(fileName,memory,false);
	g_fileManager->addFile(file);

	updateSection(++Global.Section);
}

void CDirectiveFile::initCreate(const std::wstring& fileName, u64 memory)
{
	type = Type::Create;
	std::wstring fullName = getFullPathName(fileName);

	file = new GenericAssemblerFile(fullName,memory,true);
	g_fileManager->addFile(file);

	updateSection(++Global.Section);
}

void CDirectiveFile::initCopy(const std::wstring& inputName, const std::wstring& outputName, u64 memory)
{
	type = Type::Copy;
	std::wstring fullInputName = getFullPathName(inputName);
	std::wstring fullOutputName = getFullPathName(outputName);

	if (fileExists(fullInputName) == false)
	{
		Logger::printError(Logger::Error,L"File %s not found",fullInputName);
		return;
	}
	
	file = new GenericAssemblerFile(fullOutputName,fullInputName,memory);
	g_fileManager->addFile(file);

	updateSection(++Global.Section);
}

void CDirectiveFile::initClose()
{
	type = Type::Close;
	g_fileManager->closeFile();
	updateSection(++Global.Section);
}

bool CDirectiveFile::Validate()
{
	Arch->NextSection();

	switch (type)
	{
	case Type::Open:
	case Type::Create:
	case Type::Copy:
		g_fileManager->openFile(file,true);
		return false;
	case Type::Close:
		g_fileManager->closeFile();
		return false;
	}
	
	return false;
}

void CDirectiveFile::Encode()
{
	switch (type)
	{
	case Type::Open:
	case Type::Create:
	case Type::Copy:
		g_fileManager->openFile(file,false);
		break;
	case Type::Close:
		g_fileManager->closeFile();
		break;
	}
}

void CDirectiveFile::writeTempData(TempData& tempData)
{
	std::wstring str;
	switch (type)
	{
	case Type::Open:
		str = formatString(L".open \"%s\",0x%08X",file->getFileName(),file->getOriginalHeaderSize());;
		break;
	case Type::Create:
		str = formatString(L".create \"%s\",0x%08X",file->getFileName(),file->getOriginalHeaderSize());
		break;
	case Type::Copy:
		str = formatString(L".open \"%s\",\"%s\",0x%08X",file->getOriginalFileName(),
			file->getFileName(),file->getOriginalHeaderSize());
		break;
	case Type::Close:
		str = L".close";
		break;
	}

	tempData.writeLine(g_fileManager->getVirtualAddress(),str);
}


//
// CDirectivePosition
//

CDirectivePosition::CDirectivePosition(Type type, u64 position)
	: type(type)
{
	exec();
	this->position = position;
	updateSection(++Global.Section);
}

void CDirectivePosition::exec()
{
	switch (type)
	{
	case Physical:
		g_fileManager->seekPhysical(position);
		break;
	case Virtual:
		g_fileManager->seekVirtual(position);
		break;
	}
}

bool CDirectivePosition::Validate()
{
	Arch->NextSection();
	exec();
	return false;
}

void CDirectivePosition::Encode()
{
	Arch->NextSection();
	exec();
}

void CDirectivePosition::writeTempData(TempData& tempData)
{
	switch (type)
	{
	case Physical:
		tempData.writeLine(g_fileManager->getVirtualAddress(),formatString(L".orga 0x%08X",(u32)position));
		break;
	case Virtual:
		tempData.writeLine(g_fileManager->getVirtualAddress(),formatString(L".org 0x%08X",(u32)position));
		break;
	}
}

//
// CDirectiveIncbin
//

CDirectiveIncbin::CDirectiveIncbin(const std::wstring& fileName)
	: size(0), start(0)
{
	this->fileName = getFullPathName(fileName);
	
	if (fileExists(this->fileName) == false)
	{
		Logger::printError(Logger::FatalError,L"File %s not found",this->fileName);
	}

	this->fileSize = ::fileSize(this->fileName);
}

bool CDirectiveIncbin::Validate()
{
	u64 oldStart = start;
	u64 oldSize = size;

	if (startExpression.isLoaded())
	{
		if (startExpression.evaluateInteger(start) == false)
		{
			Logger::queueError(Logger::Error,L"Invalid position expression");
			return false;
		}

		if (start > fileSize)
		{
			Logger::queueError(Logger::Error,L"Start position past end of file");
			return false;
		}
	} else {
		start = 0;
	}

	if (sizeExpression.isLoaded())
	{
		if (sizeExpression.evaluateInteger(size) == false)
		{
			Logger::queueError(Logger::Error,L"Invalid size expression");
			return false;
		}
	} else {
		size = fileSize-start;
	}

	if (start+size > fileSize)
	{
		Logger::queueError(Logger::Warning,L"Read size truncated due to file size");
		size = fileSize-start;
	}

	Arch->NextSection();
	g_fileManager->advanceMemory(size);
	return false;
}

void CDirectiveIncbin::Encode()
{
	if (size != 0)
	{
		ByteArray data = ByteArray::fromFile(fileName,(long)start,size);
		if (data.size() != size)
		{
			Logger::printError(Logger::Error,L"Could not read file \"%s\"",fileName);
			return;
		}
		g_fileManager->write(data.data(),data.size());
	}
}

void CDirectiveIncbin::writeTempData(TempData& tempData)
{
	tempData.writeLine(g_fileManager->getVirtualAddress(),formatString(L".incbin \"%s\"",fileName));
}

void CDirectiveIncbin::writeSymData(SymbolData& symData)
{
	symData.addData(g_fileManager->getVirtualAddress(),size,SymbolData::Data8);
}


//
// CDirectiveAlignFill
//

CDirectiveAlignFill::CDirectiveAlignFill(u64 value, Mode mode)
{
	this->mode = mode;
	this->value = value;
	this->finalSize = 0;
	this->fillByte = 0;
}

CDirectiveAlignFill::CDirectiveAlignFill(Expression& value, Mode mode)
	: CDirectiveAlignFill(0,mode)
{
	valueExpression = value;
}

CDirectiveAlignFill::CDirectiveAlignFill(Expression& value, Expression& fillValue, Mode mode)
	: CDirectiveAlignFill(value,mode)
{
	fillExpression = fillValue;
}

bool CDirectiveAlignFill::Validate()
{
	if (valueExpression.isLoaded())
	{
		if (valueExpression.evaluateInteger(value) == false)
		{
			Logger::printError(Logger::FatalError,L"Invalid %s",mode == Fill ? L"size" : L"alignment");
			return false;
		}
	}

	u64 oldSize = finalSize;
	u64 mod;
	switch (mode)
	{
	case Align:
		if (isPowerOfTwo(value) == false)
		{
			Logger::printError(Logger::Error,L"Invalid alignment %d",value);
			return false;
		}

		mod = g_fileManager->getVirtualAddress() % value;
		finalSize = mod ? value-mod : 0;
		break;
	case Fill:
		finalSize = value;
		break;
	}

	if (fillExpression.isLoaded())
	{
		if (fillExpression.evaluateInteger(fillByte) == false)
		{
			Logger::printError(Logger::FatalError,L"Invalid fill value");
			return false;
		}
	}

	Arch->NextSection();
	g_fileManager->advanceMemory(finalSize);

	bool result = oldSize != finalSize;
	oldSize = finalSize;
	return result;
}

void CDirectiveAlignFill::Encode()
{
	unsigned char buffer[128];
	u64 n = finalSize;

	memset(buffer,fillByte,n > 128 ? 128 : n);
	while (n > 128)
	{
		g_fileManager->write(buffer,128);
		n -= 128;
	}

	g_fileManager->write(buffer,n);
}

void CDirectiveAlignFill::writeTempData(TempData& tempData)
{
	switch (mode)
	{
	case Align:
		tempData.writeLine(g_fileManager->getVirtualAddress(),formatString(L".align 0x%08X",value));
		break;
	case Fill:
		tempData.writeLine(g_fileManager->getVirtualAddress(),formatString(L".fill 0x%08X,0x%02X",value,fillByte));
		break;
	}
}

void CDirectiveAlignFill::writeSymData(SymbolData& symData)
{
	switch (mode)
	{
	case Align:	// ?
		break;
	case Fill:
		symData.addData(g_fileManager->getVirtualAddress(),value,SymbolData::Data8);
		break;
	}
}

//
// CDirectiveHeaderSize
//

CDirectiveHeaderSize::CDirectiveHeaderSize(u64 size)
{
	headerSize = size;
	updateFile();
}

void CDirectiveHeaderSize::updateFile()
{
	AssemblerFile* openFile = g_fileManager->getOpenFile();
	if (!openFile->hasFixedVirtualAddress())
	{
		Logger::printError(Logger::Error,L"Header size not applicable for this file");
		return;
	}
	GenericAssemblerFile* file = static_cast<GenericAssemblerFile*>(openFile);
	file->setHeaderSize(headerSize);
}

bool CDirectiveHeaderSize::Validate()
{
	updateFile();
	return false;
}

void CDirectiveHeaderSize::Encode()
{
	updateFile();
}

void CDirectiveHeaderSize::writeTempData(TempData& tempData)
{
	tempData.writeLine(g_fileManager->getVirtualAddress(),formatString(L".headersize 0x%08X",headerSize));
}


//
// DirectiveObjImport
//

DirectiveObjImport::DirectiveObjImport(const std::wstring& inputName)
{
	ctor = nullptr;
	if (rel.init(inputName))
	{
		rel.exportSymbols();
	}
}

DirectiveObjImport::DirectiveObjImport(const std::wstring& inputName, const std::wstring& ctorName)
{
	if (rel.init(inputName))
	{
		rel.exportSymbols();
		ctor = rel.generateCtor(ctorName);
	}
}

bool DirectiveObjImport::Validate()
{
	bool result = false;
	if (ctor != nullptr && ctor->Validate())
		result = true;

	u64 memory = g_fileManager->getVirtualAddress();
	rel.relocate(memory);
	g_fileManager->advanceMemory((size_t)memory);

	return rel.hasDataChanged() || result;
}

void DirectiveObjImport::Encode()
{
	if (ctor != nullptr)
		ctor->Encode();

	ByteArray& data = rel.getData();
	g_fileManager->write(data.data(),data.size());
}

void DirectiveObjImport::writeTempData(TempData& tempData)
{
	if (ctor != nullptr)
		ctor->writeTempData(tempData);
}

void DirectiveObjImport::writeSymData(SymbolData& symData)
{
	if (ctor != nullptr)
		ctor->writeSymData(symData);

	rel.writeSymbols(symData);
}