/*Copyright (c) 2017 The Paradox Game Converters Project

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/


#include "HoI4World.h"
#include <fstream>
#include <algorithm>
#include <list>
#include <queue>
#include <unordered_map>
#include <boost/filesystem.hpp>
#include <boost/foreach.hpp> 
#include "ParadoxParser8859_15.h"
#include "ParadoxParserUTF8.h"
#include "Log.h"
#include "OSCompatibilityLayer.h"
#include "../Configuration.h"
#include "../V2World/Vic2Agreement.h"
#include "../V2World/V2Diplomacy.h"
#include "../V2World/V2Province.h"
#include "../V2World/V2Party.h"
#include "HoI4Faction.h"
#include "HoI4Focus.h"
#include "HoI4FocusTree.h"
#include "HoI4Relations.h"
#include "HoI4State.h"
#include "HoI4SupplyZones.h"
#include "HoI4WarCreator.h"
#include "../Mappers/CountryMapping.h"
#include "../Mappers/ProvinceMapper.h"
#include "../Mappers/StateMapper.h"



HoI4World::HoI4World(const V2World* _sourceWorld)
{
	LOG(LogLevel::Info) << "Parsing HoI4 data";
	sourceWorld = _sourceWorld;

	map<int, vector<int>> HoI4DefaultStateToProvinceMap;
	states = new HoI4States(sourceWorld);
	states->importStates(HoI4DefaultStateToProvinceMap);

	events = new HoI4Events;
	supplyZones = new HoI4SupplyZones(HoI4DefaultStateToProvinceMap);

	importStrategicRegions();
	states->convertStates();
	convertNavalBases();
	convertCountries();
	states->addLocalisations();
	convertIndustry();
	convertResources();
	supplyZones->convertSupplyZones(states);
	convertStrategicRegions();
	convertDiplomacy();
	convertTechs();
	generateLeaders();
	convertArmies();
	convertNavies();
	convertAirforces();
	determineGreatPowers();
	convertCapitalVPs();
	convertAirBases();
	createFactions();

	HoI4WarCreator warCreator;
	warCreator.generateWars(this);
	buildings = new HoI4Buildings(states->getProvinceToStateIDMap());
}


void HoI4World::importStrategicRegions()
{
	set<string> filenames;
	Utils::GetAllFilesInFolder(Configuration::getHoI4Path() + "/map/strategicregions/", filenames);
	for (auto filename: filenames)
	{
		HoI4StrategicRegion* newRegion = new HoI4StrategicRegion(filename);
		strategicRegions.insert(make_pair(newRegion->getID(), newRegion));

		for (auto province: newRegion->getOldProvinces())
		{
			provinceToStratRegionMap.insert(make_pair(province, newRegion->getID()));
		}
	}
}


void HoI4World::convertNavalBases()
{
	for (auto state: states->getStates())
	{
		state.second->convertNavalBases();
	}
}


void HoI4World::convertCountries()
{
	LOG(LogLevel::Info) << "Converting countries";

	//initLeaderTraitsMap(leaderTraits);
	governmentJobsMap governmentJobs;
	//initGovernmentJobTypes(governmentJobs);
	cultureMapping cultureMap = initCultureMap();

	personalityMap landPersonalityMap;
	personalityMap seaPersonalityMap;
	//initLeaderPersonalityMap(landPersonalityMap, seaPersonalityMap);

	backgroundMap landBackgroundMap;
	backgroundMap seaBackgroundMap;
	//initLeaderBackgroundMap(obj->getLeaves()[0], landBackgroundMap, seaBackgroundMap);

	initNamesMapping(namesMap);
	initPortraitMapping(portraitMap);

	map<int, int> leaderMap;

	for (auto sourceItr : sourceWorld->getCountries())
	{
		convertCountry(sourceItr, leaderMap, governmentJobs, cultureMap, landPersonalityMap, seaPersonalityMap, landBackgroundMap, seaBackgroundMap);
	}
	localisation.addNonenglishCountryLocalisations();
}


void HoI4World::convertCountry(pair<string, V2Country*> country, map<int, int>& leaderMap, governmentJobsMap governmentJobs, const cultureMapping& cultureMap, personalityMap& landPersonalityMap, personalityMap& seaPersonalityMap, backgroundMap& landBackgroundMap, backgroundMap& seaBackgroundMap)
{
	// don't convert rebels
	if (country.first == "REB")
	{
		return;
	}

	HoI4Country* destCountry = NULL;
	const std::string& HoI4Tag = CountryMapper::getHoI4Tag(country.first);
	if (!HoI4Tag.empty())
	{
		std::string countryFileName = '/' + country.second->getName("english") + ".txt";
		destCountry = new HoI4Country(HoI4Tag, countryFileName, this, true);
		V2Party* rulingParty = country.second->getRulingParty(sourceWorld->getParties());
		if (rulingParty == NULL)
		{
			LOG(LogLevel::Error) << "Could not find the ruling party for " << country.first << ". Were all mods correctly included?";
			exit(-1);
		}
		destCountry->initFromV2Country(*sourceWorld, country.second, rulingParty->ideology, leaderMap, governmentJobs, namesMap, portraitMap, cultureMap, landPersonalityMap, seaPersonalityMap, landBackgroundMap, seaBackgroundMap, states->getProvinceToStateIDMap(), states->getStates());
		countries.insert(make_pair(HoI4Tag, destCountry));
	}
	else
	{
		LOG(LogLevel::Warning) << "Could not convert V2 tag " << country.first << " to HoI4";
	}

	localisation.readFromCountry(country.second, HoI4Tag);
}


void HoI4World::convertIndustry()
{
	LOG(LogLevel::Info) << "Converting industry";

	addStatesToCountries();

	map<string, double> factoryWorkerRatios = calculateFactoryWorkerRatios();
	putIndustryInStates(factoryWorkerRatios);

	calculateIndustryInCountries();
	reportIndustryLevels();
}


void HoI4World::addStatesToCountries()
{
	for (auto state: states->getStates())
	{
		auto owner = countries.find(state.second->getOwner());
		if (owner != countries.end())
		{
			owner->second->addState(state.second);
		}
	}

	for (auto country: countries)
	{
		if (country.second->getStates().size() > 0)
		{
			landedCountries.insert(country);
		}
	}
}


map<string, double> HoI4World::calculateFactoryWorkerRatios()
{
	map<string, double> industrialWorkersPerCountry = getIndustrialWorkersPerCountry();
	double totalWorldWorkers = getTotalWorldWorkers(industrialWorkersPerCountry);
	map<string, double> adjustedWorkersPerCountry = adjustWorkers(industrialWorkersPerCountry, totalWorldWorkers);
	double acutalWorkerFactoryRatio = getWorldwideWorkerFactoryRatio(adjustedWorkersPerCountry, totalWorldWorkers);

	map<string, double> factoryWorkerRatios;
	for (auto country: landedCountries)
	{
		auto adjustedWorkers = adjustedWorkersPerCountry.find(country.first);
		double factories = adjustedWorkers->second * acutalWorkerFactoryRatio;

		auto Vic2Country = country.second->getSourceCountry();
		long actualWorkers = Vic2Country->getEmployedWorkers();

		if (actualWorkers > 0)
		{
			factoryWorkerRatios.insert(make_pair(country.first, factories / actualWorkers));
		}
	}

	return factoryWorkerRatios;
}


map<string, double> HoI4World::getIndustrialWorkersPerCountry()
{
	map<string, double> industrialWorkersPerCountry;
	for (auto country: landedCountries)
	{
		auto Vic2Country = country.second->getSourceCountry();
		long employedWorkers = Vic2Country->getEmployedWorkers();
		if (employedWorkers > 0)
		{
			industrialWorkersPerCountry.insert(make_pair(country.first, employedWorkers));
		}
	}

	return industrialWorkersPerCountry;
}


double HoI4World::getTotalWorldWorkers(map<string, double> industrialWorkersPerCountry)
{
	double totalWorldWorkers = 0.0;
	for (auto countryWorkers: industrialWorkersPerCountry)
	{
		totalWorldWorkers += countryWorkers.second;
	}

	return totalWorldWorkers;
}


map<string, double> HoI4World::adjustWorkers(map<string, double> industrialWorkersPerCountry, double totalWorldWorkers)
{
	double meanWorkersPerCountry = totalWorldWorkers / industrialWorkersPerCountry.size();

	map<string, double> workersDelta;
	for (auto countryWorkers: industrialWorkersPerCountry)
	{
		double delta = countryWorkers.second - meanWorkersPerCountry;
		workersDelta.insert(make_pair(countryWorkers.first, delta));
	}

	map<string, double> adjustedWorkers;
	for (auto countryWorkers: industrialWorkersPerCountry)
	{
		double delta = workersDelta.find(countryWorkers.first)->second;
		double newWorkers = countryWorkers.second - Configuration::getIndustrialShapeFactor() * delta;
		adjustedWorkers.insert(make_pair(countryWorkers.first, newWorkers));
	}

	return adjustedWorkers;
}


double HoI4World::getWorldwideWorkerFactoryRatio(map<string, double> workersInCountries, double totalWorldWorkers)
{
	double baseIndustry = 0.0;
	for (auto countryWorkers: workersInCountries)
	{
		baseIndustry += countryWorkers.second * 0.000019;
	}

	double deltaIndustry = baseIndustry - (1189 - landedCountries.size());
	double newIndustry = baseIndustry - Configuration::getIcFactor() * deltaIndustry;
	double acutalWorkerFactoryRatio = newIndustry / totalWorldWorkers;

	return acutalWorkerFactoryRatio;
}


void HoI4World::putIndustryInStates(map<string, double> factoryWorkerRatios)
{
	for (auto HoI4State : states->getStates())
	{
		auto ratioMapping = factoryWorkerRatios.find(HoI4State.second->getOwner());
		if (ratioMapping == factoryWorkerRatios.end())
		{
			continue;
		}

		HoI4State.second->convertIndustry(ratioMapping->second);
	}
}


void HoI4World::calculateIndustryInCountries()
{
	for (auto country: countries)
	{
		country.second->calculateIndustry();
	}
}


void HoI4World::reportIndustryLevels()
{
	map<string, int> countryIndustry;
	int militaryFactories = 0;
	int civilialFactories = 0;
	int dockyards = 0;
	for (auto state: states->getStates())
	{
		militaryFactories += state.second->getMilFactories();
		civilialFactories += state.second->getCivFactories();
		dockyards += state.second->getDockyards();
	}

	LOG(LogLevel::Debug) << "Total factories: " << (militaryFactories + civilialFactories + dockyards);
	LOG(LogLevel::Debug) << "\t" << militaryFactories << " military factories";
	LOG(LogLevel::Debug) << "\t" << civilialFactories << " civilian factories";
	LOG(LogLevel::Debug) << "\t" << dockyards << " dockyards";

	if (Configuration::getICStats())
	{
		reportCountryIndustry();
		reportDefaultIndustry();
	}
}


void HoI4World::reportCountryIndustry()
{
	ofstream report("convertedIndustry.csv");
	report << "tag,military factories,civilian factories,dockyards,total factories\n";
	if (report.is_open())
	{
		for (auto country: countries)
		{
			country.second->reportIndustry(report);
		}
	}
}


void HoI4World::reportDefaultIndustry()
{
	map<string, array<int, 3>> countryIndustry;

	set<string> stateFilenames;
	Utils::GetAllFilesInFolder(Configuration::getHoI4Path() + "/history/states", stateFilenames);
	for (auto stateFilename: stateFilenames)
	{
		pair<string, array<int, 3>> stateData = getDefaultStateIndustry(stateFilename);

		auto country = countryIndustry.find(stateData.first);
		if (country == countryIndustry.end())
		{
			countryIndustry.insert(make_pair(stateData.first, stateData.second));
		}
		else
		{
			country->second[0] += stateData.second[0];
			country->second[1] += stateData.second[1];
			country->second[2] += stateData.second[2];
		}
	}

	outputDefaultIndustry(countryIndustry);
}


pair<string, array<int, 3>> HoI4World::getDefaultStateIndustry(string stateFilename)
{
	Object* fileObj = parser_UTF8::doParseFile(Configuration::getHoI4Path() + "/history/states/" + stateFilename);
	if (fileObj == nullptr)
	{
		LOG(LogLevel::Error) << "Could not parse " << Configuration::getHoI4Path() << "/history/states/" << stateFilename;
		exit(-1);
	}
	auto stateObj = fileObj->getValue("state");
	auto historyObj = stateObj[0]->getValue("history");
	auto buildingsObj = historyObj[0]->getValue("buildings");

	auto civilianFactoriesObj = buildingsObj[0]->getValue("industrial_complex");
	int civilianFactories = 0;
	if (civilianFactoriesObj.size() > 0)
	{
		civilianFactories = stoi(civilianFactoriesObj[0]->getLeaf());
	}

	auto militaryFactoriesObj = buildingsObj[0]->getValue("arms_factory");
	int militaryFactories = 0;
	if (militaryFactoriesObj.size() > 0)
	{
		militaryFactories = stoi(militaryFactoriesObj[0]->getLeaf());
	}

	auto dockyardsObj = buildingsObj[0]->getValue("dockyard");
	int dockyards = 0;
	if (dockyardsObj.size() > 0)
	{
		dockyards = stoi(dockyardsObj[0]->getLeaf());
	}

	auto ownerObj = historyObj[0]->getValue("owner");
	string owner = ownerObj[0]->getLeaf();

	array<int, 3> industry = { militaryFactories, civilianFactories, dockyards };
	pair<string, array<int, 3>> stateData = make_pair(owner, industry);
	return stateData;
}


void HoI4World::outputDefaultIndustry(const map<string, array<int, 3>>& countryIndustry)
{
	ofstream report("defaultIndustry.csv");
	report << "tag,military factories,civilian factories,dockyards,total factories\n";
	if (report.is_open())
	{
		for (auto country: countryIndustry)
		{
			report << country.first << ',';
			report << country.second[0] << ',';
			report << country.second[1] << ',';
			report << country.second[2] << ',';
			report << country.second[0] + country.second[1] + country.second[2] << '\n';
		}
	}
}


void HoI4World::convertResources()
{
	auto resourceMap = importResourceMap();

	for (auto state: states->getStates())
	{
		for (auto provinceNumber: state.second->getProvinces())
		{
			auto mapping = resourceMap.find(provinceNumber);
			if (mapping != resourceMap.end())
			{
				for (auto resource: mapping->second)
				{
					state.second->addResource(resource.first, resource.second);
				}
			}
		}
	}
}


map<int, map<string, double>> HoI4World::importResourceMap() const
{
	map<int, map<string, double>> resourceMap;

	Object* fileObj = parser_UTF8::doParseFile("resources.txt");
	if (fileObj == nullptr)
	{
		LOG(LogLevel::Error) << "Could not read resources.txt";
		exit(-1);
	}

	auto resourcesObj = fileObj->getValue("resources");
	auto linksObj = resourcesObj[0]->getValue("link");
	for (auto linkObj: linksObj)
	{
		int provinceNumber = stoi(linkObj->getLeaf("province"));
		auto mapping = resourceMap.find(provinceNumber);
		if (mapping == resourceMap.end())
		{
			map<string, double> resources;
			resourceMap.insert(make_pair(provinceNumber, resources));
			mapping = resourceMap.find(provinceNumber);
		}

		auto resourcesObj = linkObj->getValue("resources");
		auto actualResources = resourcesObj[0]->getLeaves();
		for (auto resource : actualResources)
		{
			string	resourceName = resource->getKey();
			double	amount = stof(resource->getLeaf());
			mapping->second[resourceName] += amount;
		}
	}

	return resourceMap;
}


void HoI4World::output() const
{
	LOG(LogLevel::Info) << "Outputting world";

	string NFpath = "Output/" + Configuration::getOutputName() + "/common/national_focus";
	if (!Utils::TryCreateFolder(NFpath))
	{
		LOG(LogLevel::Error) << "Could not create \"Output/" + Configuration::getOutputName() + "/common/national_focus\"";
		exit(-1);
	}
	string eventpath = "Output/" + Configuration::getOutputName() + "/events";
	if (!Utils::TryCreateFolder(eventpath))
	{
		LOG(LogLevel::Error) << "Could not create \"Output/" + Configuration::getOutputName() + "/events\"";
		exit(-1);
	}

	outputCommonCountries();
	outputColorsfile();
	//outputAutoexecLua();
	outputLocalisations();
	outputHistory();
	outputMap();
	supplyZones->output();
	outputRelations();
	outputCountries();
	buildings->output();
	events->output();
}


void HoI4World::outputCommonCountries() const
{
	// Create common\countries path.
	string countriesPath = "Output/" + Configuration::getOutputName() + "/common";
	/*if (!Utils::TryCreateFolder(countriesPath))
	{
		LOG(LogLevel::Error) << "Could not create \"Output/" + Configuration::getOutputName() + "/common\"";
		exit(-1);
	}*/
	if (!Utils::TryCreateFolder(countriesPath + "/countries"))
	{
		LOG(LogLevel::Error) << "Could not create \"Output/" + Configuration::getOutputName() + "/common/countries\"";
		exit(-1);
	}
	if (!Utils::TryCreateFolder(countriesPath + "/country_tags"))
	{
		LOG(LogLevel::Error) << "Could not create \"Output/" + Configuration::getOutputName() + "/common/country_tags\"";
		exit(-1);
	}

	// Output common\countries.txt
	LOG(LogLevel::Debug) << "Writing countries file";
	FILE* allCountriesFile;
	if (fopen_s(&allCountriesFile, ("Output/" + Configuration::getOutputName() + "/common/country_tags/00_countries.txt").c_str(), "w") != 0)
	{
		LOG(LogLevel::Error) << "Could not create countries file";
		exit(-1);
	}

	for (auto countryItr : countries)
	{
		if (countryItr.second->getCapitalNum() != 0)
		{
			countryItr.second->outputToCommonCountriesFile(allCountriesFile);
		}
	}
	fprintf(allCountriesFile, "\n");
	fclose(allCountriesFile);
}


void HoI4World::outputColorsfile() const
{

	ofstream output;
	output.open(("Output/" + Configuration::getOutputName() + "/common/countries/colors.txt"));
	if (!output.is_open())
	{
		Log(LogLevel::Error) << "Could not open Output/" << Configuration::getOutputName() << "/common/countries/colors.txt";
		exit(-1);
	}

	output << "#reload countrycolors\n";
	for (auto countryItr : countries)
	{
		if (countryItr.second->getCapitalNum() != 0)
		{
			countryItr.second->outputColors(output);
		}
	}

	output.close();
}


void HoI4World::outputAutoexecLua() const
{
	// output autoexec.lua
	FILE* autoexec;
	if (fopen_s(&autoexec, ("Output/" + Configuration::getOutputName() + "/script/autoexec.lua").c_str(), "w") != 0)
	{
		LOG(LogLevel::Error) << "Could not create autoexec.lua";
		exit(-1);
	}

	ifstream sourceFile;
	sourceFile.open("autoexecTEMPLATE.lua", ifstream::in);
	if (!sourceFile.is_open())
	{
		LOG(LogLevel::Error) << "Could not open autoexecTEMPLATE.lua";
		exit(-1);
	}
	while (!sourceFile.eof())
	{
		string line;
		getline(sourceFile, line);
		fprintf(autoexec, "%s\n", line.c_str());
	}
	sourceFile.close();

	fprintf(autoexec, "\n");
	fclose(autoexec);
}


void HoI4World::outputLocalisations() const
{
	// Create localisations for all new countries. We don't actually know the names yet so we just use the tags as the names.
	LOG(LogLevel::Debug) << "Writing localisation text";
	string localisationPath = "Output/" + Configuration::getOutputName() + "/localisation";
	if (!Utils::TryCreateFolder(localisationPath))
	{
		LOG(LogLevel::Error) << "Could not create localisation folder";
		exit(-1);
	}

	localisation.output(localisationPath);
}


void HoI4World::outputMap() const
{
	LOG(LogLevel::Debug) << "Writing Map Info";

	// create the map folder
	if (!Utils::TryCreateFolder("Output/" + Configuration::getOutputName() + "/map"))
	{
		LOG(LogLevel::Error) << "Could not create \"Output/" + Configuration::getOutputName() + "/map";
		exit(-1);
	}

	// create the rocket sites file
	ofstream rocketSitesFile("Output/" + Configuration::getOutputName() + "/map/rocketsites.txt");
	if (!rocketSitesFile.is_open())
	{
		LOG(LogLevel::Error) << "Could not create Output/" << Configuration::getOutputName() << "/map/rocketsites.txt";
		exit(-1);
	}
	for (auto state : states->getStates())
	{
		auto provinces = state.second->getProvinces();
		rocketSitesFile << state.second->getID() << " = { " << *provinces.begin() << " }\n";
	}
	rocketSitesFile.close();

	// create the airports file
	ofstream airportsFile("Output/" + Configuration::getOutputName() + "/map/airports.txt");
	if (!airportsFile.is_open())
	{
		LOG(LogLevel::Error) << "Could not create Output/" << Configuration::getOutputName() << "/map/airports.txt";
		exit(-1);
	}
	for (auto state : states->getStates())
	{
		auto provinces = state.second->getProvinces();
		airportsFile << state.second->getID() << " = { " << *provinces.begin() << " }\n";
	}
	airportsFile.close();

	// output strategic regions
	if (!Utils::TryCreateFolder("Output/" + Configuration::getOutputName() + "/map/strategicregions"))
	{
		LOG(LogLevel::Error) << "Could not create \"Output/" + Configuration::getOutputName() + "/map/strategicregions";
		exit(-1);
	}
	for (auto strategicRegion : strategicRegions)
	{
		strategicRegion.second->output("Output/" + Configuration::getOutputName() + "/map/strategicregions/");
	}
}


void HoI4World::outputHistory() const
{
	states->output();

	LOG(LogLevel::Debug) << "Writing countries";
	string unitsPath = "Output/" + Configuration::getOutputName() + "/history/units";
	if (!Utils::TryCreateFolder(unitsPath))
	{
		LOG(LogLevel::Error) << "Could not create \"Output/" + Configuration::getOutputName() + "/history/units";
		exit(-1);
	}
	/*for (auto countryItr: countries)
	{
		countryItr.second->output(states->getStates(), );
	}*/

	LOG(LogLevel::Debug) << "Writing diplomacy";
	//diplomacy.output();
}


void HoI4World::getProvinceLocalizations(const string& file)
{
	ifstream read;
	string line;
	read.open(file);
	while (read.good() && !read.eof())
	{
		getline(read, line);
		if (line.substr(0, 4) == "PROV" && isdigit(line[4]))
		{
			int position = line.find_first_of(';');
			int num = stoi(line.substr(4, position - 4));
			string name = line.substr(position + 1, line.find_first_of(';', position + 1) - position - 1);
			provinces[num]->setName(name);
		}
	}
	read.close();
}


void HoI4World::outputCountries() const
{
	for (auto country : countries)
	{
		country.second->output(states->getStates(), factions);
	}
}


void HoI4World::convertStrategicRegions()
{
	// assign the states to strategic regions
	for (auto state : states->getStates())
	{
		// figure out which strategic regions are represented
		map<int, int> usedRegions;	// region ID -> number of provinces in that region
		for (auto province : state.second->getProvinces())
		{
			auto mapping = provinceToStratRegionMap.find(province);
			if (mapping == provinceToStratRegionMap.end())
			{
				LOG(LogLevel::Warning) << "Province " << province << " had no original strategic region";
				continue;
			}

			auto usedRegion = usedRegions.find(mapping->second);
			if (usedRegion == usedRegions.end())
			{
				usedRegions.insert(make_pair(mapping->second, 1));
			}
			else
			{
				usedRegion->second++;
			}

			provinceToStratRegionMap.erase(mapping);
		}

		// pick the most represented strategic region
		int mostProvinces = 0;
		int bestRegion = 0;
		for (auto region : usedRegions)
		{
			if (region.second > mostProvinces)
			{
				bestRegion = region.first;
				mostProvinces = region.second;
			}
		}

		// add the state's province to the region
		auto region = strategicRegions.find(bestRegion);
		if (region == strategicRegions.end())
		{
			LOG(LogLevel::Warning) << "Strategic region " << bestRegion << " was not in the list of regions.";
			continue;
		}
		for (auto province : state.second->getProvinces())
		{
			region->second->addNewProvince(province);
		}
	}

	// add leftover provinces back to their strategic regions
	for (auto mapping : provinceToStratRegionMap)
	{
		auto region = strategicRegions.find(mapping.second);
		if (region == strategicRegions.end())
		{
			LOG(LogLevel::Warning) << "Strategic region " << mapping.second << " was not in the list of regions.";
			continue;
		}
		region->second->addNewProvince(mapping.first);
	}
}


void HoI4World::convertTechs()
{
	LOG(LogLevel::Info) << "Converting techs";

	map<string, vector<pair<string, int> > > techTechMap;
	map<string, vector<pair<string, int> > > invTechMap;

	// build tech maps - the code is ugly so the file can be pretty
	Object* obj = parser_UTF8::doParseFile("tech_mapping.txt");
	vector<Object*> objs = obj->getValue("tech_map");
	if (objs.size() < 1)
	{
		LOG(LogLevel::Error) << "Could not read tech map!";
		exit(1);
	}
	objs = objs[0]->getValue("link");
	for (auto itr : objs)
	{
		vector<string> keys = itr->getKeys();
		int status = 0; // 0 = unhandled, 1 = tech, 2 = invention
		vector<pair<string, int> > targetTechs;
		string tech = "";
		for (auto master : keys)
		{
			if ((status == 0) && (master == "v2_inv"))
			{
				tech = itr->getLeaf("v2_inv");
				status = 2;
			}
			else if ((status == 0) && (master == "v2_tech"))
			{
				tech = itr->getLeaf("v2_tech");
				status = 1;
			}
			else
			{
				int value = stoi(itr->getLeaf(master));
				targetTechs.push_back(pair<string, int>(master, value));
			}
		}
		switch (status)
		{
		case 0:
			LOG(LogLevel::Error) << "unhandled tech link with first key " << keys[0] << "!";
			break;
		case 1:
			techTechMap[tech] = targetTechs;
			break;
		case 2:
			invTechMap[tech] = targetTechs;
			break;
		}
	}


	for (auto dstCountry : countries)
	{
		const V2Country*	sourceCountry = dstCountry.second->getSourceCountry();
		vector<string>	techs = sourceCountry->getTechs();

		for (auto techName : techs)
		{
			auto mapItr = techTechMap.find(techName);
			if (mapItr != techTechMap.end())
			{
				for (auto HoI4TechItr : mapItr->second)
				{
					dstCountry.second->setTechnology(HoI4TechItr.first, HoI4TechItr.second);
				}
			}
		}

		auto srcInventions = sourceCountry->getInventions();
		for (auto invItr : srcInventions)
		{
			auto mapItr = invTechMap.find(invItr);
			if (mapItr == invTechMap.end())
			{
				continue;
			}
			else
			{
				for (auto HoI4TechItr : mapItr->second)
				{
					dstCountry.second->setTechnology(HoI4TechItr.first, HoI4TechItr.second);
				}
			}
		}
	}
}


vector<int> HoI4World::getPortLocationCandidates(const vector<int>& locationCandidates, const HoI4AdjacencyMapping& HoI4AdjacencyMap)
{
	vector<int> portLocationCandidates = getPortProvinces(locationCandidates);
	if (portLocationCandidates.size() == 0)
	{
		// if none of the mapped provinces are ports, try to push the navy out to sea
		for (auto candidate : locationCandidates)
		{
			if (HoI4AdjacencyMap.size() > static_cast<unsigned int>(candidate))
			{
				auto newCandidates = HoI4AdjacencyMap[candidate];
				for (auto newCandidate : newCandidates)
				{
					auto candidateProvince = provinces.find(newCandidate.to);
					if (candidateProvince == provinces.end())	// if this was not an imported province but has an adjacency, we can assume it's a sea province
					{
						portLocationCandidates.push_back(newCandidate.to);
					}
				}
			}
		}
	}
	return portLocationCandidates;
}


vector<int> HoI4World::getPortProvinces(const vector<int>& locationCandidates)
{
	vector<int> newLocationCandidates;
	for (auto litr : locationCandidates)
	{
		map<int, HoI4Province*>::const_iterator provinceItr = provinces.find(litr);
		if ((provinceItr != provinces.end()) && (provinceItr->second->hasNavalBase()))
		{
			newLocationCandidates.push_back(litr);
		}
	}

	return newLocationCandidates;
}


int HoI4World::getAirLocation(HoI4Province* locationProvince, const HoI4AdjacencyMapping& HoI4AdjacencyMap, string owner)
{
	queue<int>		openProvinces;
	map<int, int>	closedProvinces;
	openProvinces.push(locationProvince->getNum());
	closedProvinces.insert(make_pair(locationProvince->getNum(), locationProvince->getNum()));
	while (openProvinces.size() > 0)
	{
		int provNum = openProvinces.front();
		openProvinces.pop();

		auto province = provinces.find(provNum);
		if ((province != provinces.end()) && (province->second->getOwner() == owner) && (province->second->getAirBase() > 0))
		{
			return provNum;
		}
		else
		{
			auto adjacencies = HoI4AdjacencyMap[provNum];
			for (auto thisAdjacency : adjacencies)
			{
				auto closed = closedProvinces.find(thisAdjacency.to);
				if (closed == closedProvinces.end())
				{
					openProvinces.push(thisAdjacency.to);
					closedProvinces.insert(make_pair(thisAdjacency.to, thisAdjacency.to));
				}
			}
		}
	}

	return -1;
}


void HoI4World::generateLeaders()
{
	LOG(LogLevel::Info) << "Generating Leaders";

	for (auto country : countries)
	{
		country.second->generateLeaders(leaderTraits, namesMap, portraitMap);
	}
}


void HoI4World::convertArmies()
{
	LOG(LogLevel::Info) << "Converting armies";

	for (auto country : countries)
	{
		country.second->convertArmyDivisions();
	}
}


void HoI4World::convertNavies()
{
	LOG(LogLevel::Info) << "Converting navies";

	for (auto country : countries)
	{
		country.second->convertNavy(states->getStates());
	}
}


void HoI4World::convertAirforces()
{
	LOG(LogLevel::Info) << "Converting air forces";

	for (auto country : countries)
	{
		country.second->convertAirforce();
	}
}


void HoI4World::determineGreatPowers()
{
	for (auto greatPowerVic2Tag: sourceWorld->getGreatPowers())
	{
		string greatPowerTag = CountryMapper::getHoI4Tag(greatPowerVic2Tag);
		auto greatPower = countries.find(greatPowerTag);
		if (greatPower != countries.end())
		{
			greatPowers.push_back(greatPower->second);
			greatPower->second->setGreatPower();
		}
	}
}


void HoI4World::convertCapitalVPs()
{
	LOG(LogLevel::Info) << "Adding bonuses to capitals";

	addBasicCapitalVPs();
	addGreatPowerVPs();
	addStrengthVPs();
}


void HoI4World::convertAirBases()
{
	addBasicAirBases();
	addCapitalAirBases();
	addGreatPowerAirBases();
}


void HoI4World::addBasicAirBases()
{
	for (auto state: states->getStates())
	{
		int numFactories = (state.second->getCivFactories() + state.second->getMilFactories()) / 4;
		state.second->addAirBase(numFactories);

		if (state.second->getInfrastructure() > 5)
		{
			state.second->addAirBase(1);
		}
	}
}


void HoI4World::addCapitalAirBases()
{
	for (auto country: countries)
	{
		auto capitalState = country.second->getCapital();
		if (capitalState != nullptr)
		{
			capitalState->addAirBase(5);
		}
	}
}


void HoI4World::addGreatPowerAirBases()
{
	for (auto greatPower: greatPowers)
	{
		auto capitalState = greatPower->getCapital();
		if (capitalState != nullptr)
		{
			capitalState->addAirBase(5);
		}
	}
}


void HoI4World::addBasicCapitalVPs()
{
	for (auto countryItr: countries)
	{
		countryItr.second->addVPsToCapital(5);
	}
}


void HoI4World::addGreatPowerVPs()
{
	for (auto greatPower: greatPowers)
	{
		greatPower->addVPsToCapital(5);
	}
}


void HoI4World::addStrengthVPs()
{
	double greatestStrength = getStrongestCountryStrength();
	for (auto country: countries)
	{
		int VPs = calculateStrengthVPs(country.second, greatestStrength);
		country.second->addVPsToCapital(VPs);
	}
}


double HoI4World::getStrongestCountryStrength()
{
	double greatestStrength = 0.0;
	for (auto country: countries)
	{
		double currentStrength = country.second->getStrengthOverTime(1.0);
		if (currentStrength > greatestStrength)
		{
			greatestStrength = currentStrength;
		}
	}

	return greatestStrength;
}


int HoI4World::calculateStrengthVPs(HoI4Country* country, double greatestStrength)
{
	double relativeStrength = country->getStrengthOverTime(1.0) / greatestStrength;
	return static_cast<int>(relativeStrength * 30.0);
}


void HoI4World::convertDiplomacy()
{
	LOG(LogLevel::Info) << "Converting diplomacy";
	convertAgreements();
	convertRelations();
}


void HoI4World::convertAgreements()
{
	for (auto agreement : sourceWorld->getDiplomacy()->getAgreements())
	{
		string HoI4Tag1 = CountryMapper::getHoI4Tag(agreement->country1);
		if (HoI4Tag1.empty())
		{
			continue;
		}
		string HoI4Tag2 = CountryMapper::getHoI4Tag(agreement->country2);
		if (HoI4Tag2.empty())
		{
			continue;
		}

		map<string, HoI4Country*>::iterator HoI4Country1 = countries.find(HoI4Tag1);
		map<string, HoI4Country*>::iterator HoI4Country2 = countries.find(HoI4Tag2);
		if (HoI4Country1 == countries.end())
		{
			LOG(LogLevel::Warning) << "HoI4 country " << HoI4Tag1 << " used in diplomatic agreement doesn't exist";
			continue;
		}
		if (HoI4Country2 == countries.end())
		{
			LOG(LogLevel::Warning) << "HoI4 country " << HoI4Tag2 << " used in diplomatic agreement doesn't exist";
			continue;
		}

		// shared diplo types
		if ((agreement->type == "alliance") || (agreement->type == "vassa"))
		{
			// copy agreement
			HoI4Agreement* HoI4a = new HoI4Agreement;
			HoI4a->country1 = HoI4Tag1;
			HoI4a->country2 = HoI4Tag2;
			HoI4a->start_date = agreement->start_date;
			HoI4a->type = agreement->type;
			diplomacy.addAgreement(HoI4a);

			if (agreement->type == "alliance")
			{
				HoI4Country1->second->editAllies().insert(HoI4Tag2);
				HoI4Country2->second->editAllies().insert(HoI4Tag1);
			}
		}
	}
}


void HoI4World::convertRelations()
{
	for (auto country: countries)
	{
		for (auto relationItr: country.second->getRelations())
		{
			HoI4Agreement* HoI4a = new HoI4Agreement;
			if (country.first < relationItr.first) // Put it in order to eliminate duplicate relations entries
			{
				HoI4a->country1 = country.first;
				HoI4a->country2 = relationItr.first;
			}
			else
			{
				HoI4a->country2 = relationItr.first;
				HoI4a->country1 = country.first;
			}

			HoI4a->value = relationItr.second->getRelations();
			HoI4a->start_date = date("1930.1.1"); // Arbitrary date
			HoI4a->type = "relation";
			diplomacy.addAgreement(HoI4a);

			if (relationItr.second->getGuarantee())
			{
				HoI4Agreement* HoI4a = new HoI4Agreement;
				HoI4a->country1 = country.first;
				HoI4a->country2 = relationItr.first;
				HoI4a->start_date = date("1930.1.1"); // Arbitrary date
				HoI4a->type = "guarantee";
				diplomacy.addAgreement(HoI4a);
			}
			if (relationItr.second->getSphereLeader())
			{
				HoI4Agreement* HoI4a = new HoI4Agreement;
				HoI4a->country1 = country.first;
				HoI4a->country2 = relationItr.first;
				HoI4a->start_date = date("1930.1.1"); // Arbitrary date
				HoI4a->type = "sphere";
				diplomacy.addAgreement(HoI4a);
			}
		}
	}
}


void HoI4World::outputRelations() const
{
	if (!Utils::TryCreateFolder("Output/" + Configuration::getOutputName() + "/common/opinion_modifiers"))
	{
		Log(LogLevel::Error) << "Could not create Output/" + Configuration::getOutputName() + "/common/opinion_modifiers/";
		exit(-1);
	}

	ofstream out("Output/" + Configuration::getOutputName() + "/common/opinion_modifiers/01_opinion_modifiers.txt");
	if (!out.is_open())
	{
		LOG(LogLevel::Error) << "Could not create 01_opinion_modifiers.txt.";
		exit(-1);
	}

	out << "opinion_modifiers = {\n";
	for (auto country: countries)
	{
		for (auto relation: country.second->getRelations())
		{
			if (country.first == relation.first)
			{
				continue;
			}

			out << country.first << "_" << relation.first << " = {\n";
			out << "\tvalue = " << relation.second->getRelations() << "\n";
			out << "}\n";
		}
	}

	out << "}\n";

	out.close();
}


void HoI4World::createFactions()
{
	LOG(LogLevel::Info) << "Creating Factions";

	ofstream factionsLog("factions-logs.csv");
	factionsLog << "name,government,initial strength,factory strength per year,factory strength by 1939\n";

	for (auto leader: greatPowers)
	{
		if (leader->isInFaction())
		{
			continue;
		}
		factionsLog << "\n";

		vector<HoI4Country*> factionMembers;
		factionMembers.push_back(leader);

		string leaderGovernment = leader->getGovernment();
		logFactionMember(factionsLog, leader);
		double factionMilStrength = leader->getStrengthOverTime(3.0);

		for (auto allyTag: leader->getAllies())
		{
			auto ally = countries.find(allyTag);
			if (ally == countries.end())
			{
				continue;
			}

			HoI4Country* allycountry = ally->second;
			string allygovernment = allycountry->getGovernment();
			string sphereLeader = returnSphereLeader(allycountry);

			if ((sphereLeader == leader->getTag()) || ((sphereLeader == "") && governmentsAllowFaction(leaderGovernment, allygovernment)))
			{
				logFactionMember(factionsLog, allycountry);
				factionMembers.push_back(allycountry);

				factionMilStrength += allycountry->getStrengthOverTime(1.0);
			}
		}

		HoI4Faction* newFaction = new HoI4Faction(leader, factionMembers);
		for (auto member: factionMembers)
		{
			member->setFaction(newFaction);
		}
		factions.push_back(newFaction);

		factionsLog << "Faction Strength in 1939," << factionMilStrength << "\n";
	}

	factionsLog.close();
}


void HoI4World::logFactionMember(ofstream& factionsLog, const HoI4Country* member)
{
	factionsLog << member->getSourceCountry()->getName("english") << ",";
	factionsLog << member->getGovernment() << ",";
	factionsLog << member->getMilitaryStrength() << ",";
	factionsLog << member->getEconomicStrength(1.0) << ",";
	factionsLog << member->getEconomicStrength(3.0) << "\n";
}


string HoI4World::returnSphereLeader(HoI4Country* possibleSphereling)
{
	for (auto greatPower: greatPowers)
	{
		auto relations = greatPower->getRelations();
		auto relation = relations.find(possibleSphereling->getTag());
		if (relation != relations.end())
		{
			if (relation->second->getSphereLeader())
			{
				return greatPower->getTag();
			}
		}
	}

	return "";
}


bool HoI4World::governmentsAllowFaction(string leaderGovernment, string allyGovernment)
{
	if (leaderGovernment == allyGovernment)
	{
		return true;
	}
	else if (leaderGovernment == "absolute_monarchy" && (allyGovernment == "fascism" || allyGovernment == "democratic" || allyGovernment == "prussian_constitutionalism" || allyGovernment == "hms_government"))
	{
		return true;
	}
	else if (leaderGovernment == "democratic" && (allyGovernment == "hms_government" || allyGovernment == "absolute_monarchy" || allyGovernment == "prussian_constitutionalism"))
	{
		return true;
	}
	else if (leaderGovernment == "prussian_constitutionalism" && (allyGovernment == "hms_government" || allyGovernment == "absolute_monarchy" || allyGovernment == "democratic" || allyGovernment == "fascism"))
	{
		return true;
	}
	else if (leaderGovernment == "hms_government" && (allyGovernment == "democratic" || allyGovernment == "absolute_monarchy" || allyGovernment == "prussian_constitutionalism"))
	{
		return true;
	}
	else if (leaderGovernment == "communism" && (allyGovernment == "syndicalism"))
	{
		return true;
	}
	else if (leaderGovernment == "syndicalism" && (allyGovernment == "communism" || allyGovernment == "fascism"))
	{
		return true;
	}
	else if (leaderGovernment == "fascism" && (allyGovernment == "syndicalism" || allyGovernment == "absolute_monarchy" || allyGovernment == "prussian_constitutionalism"))
	{
		return true;
	}
	else
	{
		return false;
	}
}