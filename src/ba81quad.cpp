/*
  Copyright 2012-2014, 2016 Joshua Nathaniel Pritikin and contributors

  This is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "ba81quad.h"

ba81NormalQuad::ba81NormalQuad(struct ifaGroup *ig) : ig(*ig), abilities(-1)
{
	setOne(1);
	layers.resize(1, layer(this));
}

ifaGroup::ifaGroup(int cores, bool _twotier) :
	Rdata(NULL), numThreads(cores), qwidth(6.0), qpoints(49), quad(this),
	twotier(_twotier), maxAbilities(0), mean(0), cov(0), dataRowNames(0),
	weightColumnName(0), rowWeight(0), minItemsPerScore(NA_INTEGER),
	outcomeProb(0), excludedPatterns(-1)
{}

// The idea here is to avoid denormalized values if they are
// enabled (5e-324 vs 2e-308).  It would be bad if results
// changed depending on the denormalization setting.
// Moreover, we don't lose too much even if denormalized
// values are disabled. This mainly affects models with
// more than a thousand items.
const double ifaGroup::SmallestPatternLik = 1e16 * std::numeric_limits<double>::min();  //constexpr

ifaGroup::~ifaGroup()
{
	Free(outcomeProb);
}

void ifaGroup::importSpec(SEXP slotValue)
{
	for (int sx=0; sx < Rf_length(slotValue); ++sx) {
		SEXP model = VECTOR_ELT(slotValue, sx);
		if (!OBJECT(model)) {
			Rf_error("Item models must inherit rpf.base");
		}
		SEXP Rspec;
		ScopedProtect p1(Rspec, R_do_slot(model, Rf_install("spec")));
		spec.push_back(REAL(Rspec));
	}

	dataColumns.reserve(spec.size());
	itemOutcomes.reserve(spec.size());
	cumItemOutcomes.reserve(spec.size());

	impliedParamRows = 0;
	totalOutcomes = 0;
	maxOutcomes = 0;
	itemDims = -1;
	for (int cx = 0; cx < numItems(); cx++) {
		const double *ispec = spec[cx];
		int id = ispec[RPF_ISpecID];
		int dims = ispec[RPF_ISpecDims];
		if (itemDims == -1) {
			itemDims = dims;
		} else if (dims != itemDims) {
			Rf_error("All items must have the same number of factors (%d != %d)",
				 itemDims, dims);
		}
		int no = ispec[RPF_ISpecOutcomes];
		itemOutcomes.push_back(no);
		cumItemOutcomes.push_back(totalOutcomes);
		maxOutcomes = std::max(maxOutcomes, no);
		totalOutcomes += no;

		int numParam = (*Glibrpf_model[id].numParam)(ispec);
		if (impliedParamRows < numParam)
			impliedParamRows = numParam;
	}
}

void ifaGroup::verifyFactorNames(SEXP mat, const char *matName)
{
	static const char *dimname[] = { "row", "col" };

	SEXP dimnames;
	Rf_protect(dimnames = Rf_getAttrib(mat, R_DimNamesSymbol));
	if (!Rf_isNull(dimnames) && Rf_length(dimnames) == 2) {
		for (int dx=0; dx < 2; ++dx) {
			SEXP names;
			Rf_protect(names = VECTOR_ELT(dimnames, dx));
			if (!Rf_length(names)) continue;
			if (int(factorNames.size()) != Rf_length(names)) {
				Rf_error("%s %snames must be length %d",
					 matName, dimname[dx], (int) factorNames.size());
			}
			int nlen = Rf_length(names);
			for (int nx=0; nx < nlen; ++nx) {
				const char *name = CHAR(STRING_ELT(names, nx));
				if (strEQ(factorNames[nx].c_str(), name)) continue;
				Rf_error("%s %snames[%d] is '%s', does not match factor name '%s'",
					 matName, dimname[dx], 1+nx, name, factorNames[nx].c_str());
			}
		}
	}
}

void ifaGroup::learnMaxAbilities()
{
	maxAbilities = 0;
	Eigen::ArrayXi loadings(itemDims);
	loadings.setZero();
	for (int cx = 0; cx < numItems(); cx++) {
		for (int dx=0; dx < itemDims; ++dx) {
			if (getItemParam(cx)[dx] != 0) loadings[dx] += 1;
		}
	}
	maxAbilities = (loadings != 0).count();
	if (itemDims != maxAbilities) {
		for (int lx=0; lx < itemDims; ++lx) {
			if (loadings[lx] == 0) Rf_error("Factor %d does not load on any items", 1+lx);
		}
	}
}

void ifaGroup::import(SEXP Rlist)
{
	SEXP argNames;
	Rf_protect(argNames = Rf_getAttrib(Rlist, R_NamesSymbol));
	if (Rf_length(Rlist) != Rf_length(argNames)) {
		Rf_error("All list elements must be named");
	}

	std::vector<const char *> dataColNames;

	paramRows = -1;
	int pmatCols=-1;
	int mips = 1;
	int dataRows = 0;
	SEXP Rmean=0, Rcov=0;

	for (int ax=0; ax < Rf_length(Rlist); ++ax) {
		const char *key = R_CHAR(STRING_ELT(argNames, ax));
		SEXP slotValue = VECTOR_ELT(Rlist, ax);
		if (strEQ(key, "spec")) {
			importSpec(slotValue);
		} else if (strEQ(key, "param")) {
			if (!Rf_isReal(slotValue)) Rf_error("'param' must be a numeric matrix of item parameters");
			param = REAL(slotValue);
			getMatrixDims(slotValue, &paramRows, &pmatCols);

			SEXP dimnames;
			Rf_protect(dimnames = Rf_getAttrib(slotValue, R_DimNamesSymbol));
			if (!Rf_isNull(dimnames) && Rf_length(dimnames) == 2) {
				SEXP names;
				Rf_protect(names = VECTOR_ELT(dimnames, 0));
				int nlen = Rf_length(names);
				factorNames.resize(nlen);
				for (int nx=0; nx < nlen; ++nx) {
					factorNames[nx] = CHAR(STRING_ELT(names, nx));
				}
				Rf_protect(names = VECTOR_ELT(dimnames, 1));
				nlen = Rf_length(names);
				itemNames.resize(nlen);
				for (int nx=0; nx < nlen; ++nx) {
					itemNames[nx] = CHAR(STRING_ELT(names, nx));
				}
			}
		} else if (strEQ(key, "mean")) {
			Rmean = slotValue;
			if (!Rf_isReal(slotValue)) Rf_error("'mean' must be a numeric vector or matrix");
			mean = REAL(slotValue);
		} else if (strEQ(key, "cov")) {
			Rcov = slotValue;
			if (!Rf_isReal(slotValue)) Rf_error("'cov' must be a numeric matrix");
			cov = REAL(slotValue);
		} else if (strEQ(key, "data")) {
			Rdata = slotValue;
			dataRows = Rf_length(VECTOR_ELT(Rdata, 0));

			SEXP names;
			Rf_protect(names = Rf_getAttrib(Rdata, R_NamesSymbol));
			int nlen = Rf_length(names);
			dataColNames.reserve(nlen);
			for (int nx=0; nx < nlen; ++nx) {
				dataColNames.push_back(CHAR(STRING_ELT(names, nx)));
			}
			Rf_protect(dataRowNames = Rf_getAttrib(Rdata, R_RowNamesSymbol));
		} else if (strEQ(key, "weightColumn")) {
			if (Rf_length(slotValue) != 1) {
				Rf_error("You can only have one weightColumn");
			}
			weightColumnName = CHAR(STRING_ELT(slotValue, 0));
		} else if (strEQ(key, "qwidth")) {
			qwidth = Rf_asReal(slotValue);
		} else if (strEQ(key, "qpoints")) {
			qpoints = Rf_asInteger(slotValue);
		} else if (strEQ(key, "minItemsPerScore")) {
			mips = Rf_asInteger(slotValue);
		} else {
			// ignore
		}
	}

	learnMaxAbilities();

	if (maxAbilities < (int) factorNames.size())
		factorNames.resize(maxAbilities);

	if (int(factorNames.size()) < maxAbilities) {
		factorNames.reserve(maxAbilities);
		const int SMALLBUF = 10;
		char buf[SMALLBUF];
		while (int(factorNames.size()) < maxAbilities) {
			snprintf(buf, SMALLBUF, "s%d", int(factorNames.size()) + 1);
			factorNames.push_back(CHAR(Rf_mkChar(buf)));
		}
	}

	if (Rmean) {
		if (Rf_isMatrix(Rmean)) {
			int nrow, ncol;
			getMatrixDims(Rmean, &nrow, &ncol);
			if (!(nrow * ncol == maxAbilities && (nrow==1 || ncol==1))) {
				Rf_error("mean must be a column or row matrix of length %d", maxAbilities);
			}
		} else {
			if (Rf_length(Rmean) != maxAbilities) {
				Rf_error("mean must be a vector of length %d", maxAbilities);
			}
		}

		verifyFactorNames(Rmean, "mean");
	}

	if (Rcov) {
		if (Rf_isMatrix(Rcov)) {
			int nrow, ncol;
			getMatrixDims(Rcov, &nrow, &ncol);
			if (nrow != maxAbilities || ncol != maxAbilities) {
				Rf_error("cov must be %dx%d matrix", maxAbilities, maxAbilities);
			}
		} else {
			if (Rf_length(Rcov) != 1) {
				Rf_error("cov must be %dx%d matrix", maxAbilities, maxAbilities);
			}
		}

		verifyFactorNames(Rcov, "cov");
	}

	setLatentDistribution(maxAbilities, mean, cov);

	setMinItemsPerScore(mips);

	if (numItems() != pmatCols) {
		Rf_error("item matrix implies %d items but spec is length %d",
			 pmatCols, numItems());
	}

	if (Rdata) {
		if (itemNames.size() == 0) Rf_error("Item matrix must have colnames");
		for (int ix=0; ix < numItems(); ++ix) {
			bool found=false;
			for (int dc=0; dc < int(dataColNames.size()); ++dc) {
				if (strEQ(itemNames[ix], dataColNames[dc])) {
					SEXP col = VECTOR_ELT(Rdata, dc);
					if (!Rf_isFactor(col)) {
						if (TYPEOF(col) == INTSXP) {
							Rf_error("Column '%s' is an integer but "
								 "not an ordered factor",
								 dataColNames[dc]);
						} else {
							Rf_error("Column '%s' is of type %s; expecting an "
								 "ordered factor (integer)",
								 dataColNames[dc], Rf_type2char(TYPEOF(col)));
						}
					}
					dataColumns.push_back(INTEGER(col));
					found=true;
					break;
				}
			}
			if (!found) {
				Rf_error("Cannot find item '%s' in data", itemNames[ix]);
			}
		}
		if (weightColumnName) {
			for (int dc=0; dc < int(dataColNames.size()); ++dc) {
				if (strEQ(weightColumnName, dataColNames[dc])) {
					SEXP col = VECTOR_ELT(Rdata, dc);
					if (TYPEOF(col) != REALSXP) {
						Rf_error("Column '%s' is of type %s; expecting type numeric (double)",
							 dataColNames[dc], Rf_type2char(TYPEOF(col)));
					}
					rowWeight = REAL(col);
					break;
				}
			}
			if (!rowWeight) {
				Rf_error("Cannot find weight column '%s'", weightColumnName);
			}
		}
		rowMap.reserve(dataRows);
		for (int rx=0; rx < dataRows; ++rx) rowMap.push_back(rx);
	}

	Eigen::Map< Eigen::ArrayXXd > Eparam(param, paramRows, numItems());
	Eigen::Map< Eigen::VectorXd > meanVec(mean, maxAbilities);
	Eigen::Map< Eigen::MatrixXd > covMat(cov, maxAbilities, maxAbilities);

	quad.setStructure(qwidth, qpoints, Eparam, meanVec, covMat);

	if (paramRows < impliedParamRows) {
		Rf_error("At least %d rows are required in the item parameter matrix, only %d found",
			 impliedParamRows, paramRows);
	}
	
	quad.refresh(meanVec, covMat);
}

void ifaGroup::setLatentDistribution(int dims, double *_mean, double *_cov)
{
	maxAbilities = dims;

	if (maxAbilities < 0) Rf_error("maxAbilities must be non-negative");

	if (!mean) {
		mean = (double *) R_alloc(maxAbilities, sizeof(double));
		memset(mean, 0, maxAbilities * sizeof(double));
	} else {
		mean = _mean;
	}

	if (!cov) {
		cov = (double *) R_alloc(maxAbilities * maxAbilities, sizeof(double));
		Eigen::Map< Eigen::MatrixXd > covMat(cov, maxAbilities, maxAbilities);
		covMat.setIdentity();
	} else {
		cov = _cov;
	}
}

template <typename T1, typename T2, typename T3>
void ba81NormalQuad::setStructure(double Qwidth, int Qpoints,
				  Eigen::ArrayBase<T1> &param,
				  Eigen::MatrixBase<T2> &mean, Eigen::MatrixBase<T3> &cov)
{
	hasBifactorStructure = false;
	width = Qwidth;
	gridSize = Qpoints;

	if (int(Qpoint.size()) != gridSize) {
		Qpoint.clear();
		Qpoint.reserve(gridSize);
		double qgs = gridSize-1;
		for (int px=0; px < gridSize; ++px) {
			Qpoint.push_back(px * 2 * width / qgs - width);
		}
	}

	// split into independent layers TODO
	layers.clear();
	layers.resize(1, layer(this));

	abilities = mean.cols();

	if (!abilities) {
		// Qpoint.clear();  // remove? TODO
		// Qpoint.reserve(1);
		// Qpoint.push_back(0);
		gridSize = 1;
	}

	// subset param, mean, cov, etc
	layers[0].setStructure(param, mean, cov);

	totalQuadPoints = 0;
	weightTableSize = 0;
	int offset = 0;
	for (size_t lx=0; lx < layers.size(); ++lx) {
		layers[lx].abilitiesOffset = offset;
		offset += layers[lx].abilities;
		totalQuadPoints += layers[lx].totalQuadPoints;
		weightTableSize += layers[lx].weightTableSize;
	}
}

template <typename T1, typename T2, typename T3>
void ba81NormalQuad::layer::setStructure(Eigen::ArrayBase<T1> &param,
					 Eigen::MatrixBase<T2> &mean, Eigen::MatrixBase<T3> &cov)
{
	if (mean.size() == 0) {
		numSpecific = 0;
		primaryDims = 0;
		maxDims = 1;
		abilities = 0;
		totalQuadPoints = 1;
		totalPrimaryPoints = 1;
		weightTableSize = 1;
		wherePrep.clear();
		wherePrep.push_back(0);
		return;
	}

	std::vector<double> &Qpoint = quad->Qpoint;

	numSpecific = 0;
	
	if (quad->ig.twotier) detectTwoTier(param, mean, cov);
	if (numSpecific) quad->hasBifactorStructure = true;

	primaryDims = cov.cols() - numSpecific;
	maxDims = primaryDims + (numSpecific? 1 : 0);
	abilities = primaryDims + numSpecific;

	totalQuadPoints = 1;
	for (int dx=0; dx < maxDims; dx++) {
		totalQuadPoints *= quad->gridSize;
	}

	totalPrimaryPoints = totalQuadPoints;
	weightTableSize = totalQuadPoints;

	if (numSpecific) {
		totalPrimaryPoints /= quad->gridSize;
		weightTableSize *= numSpecific;
	}

	wherePrep.clear();
	wherePrep.reserve(totalQuadPoints * maxDims);

	Eigen::VectorXi abscissa(maxDims);
	for (int qx=0; qx < totalQuadPoints; qx++) {
		decodeLocation(qx, quad->gridSize, abscissa);
		for (int dx=0; dx < maxDims; dx++) {
			wherePrep.push_back(Qpoint[abscissa[dx]]);
		}
	}

	whereGram.resize(triangleLoc1(maxDims), totalQuadPoints);

	for (int qx=0; qx < totalQuadPoints; qx++) {
		double *wh = wherePrep.data() + qx * maxDims;
		gramProduct(wh, maxDims, &whereGram.coeffRef(0, qx));
	}
}

template <typename T1, typename T2, typename T3>
void ba81NormalQuad::layer::detectTwoTier(Eigen::ArrayBase<T1> &param,
					  Eigen::MatrixBase<T2> &mean, Eigen::MatrixBase<T3> &cov)
{
	if (mean.cols() < 3) return;

	std::vector<int> orthogonal;

	Eigen::Matrix<Eigen::DenseIndex, Eigen::Dynamic, 1>
		numCov((cov.array() != 0.0).matrix().colwise().count());
	std::vector<int> candidate;
	for (int fx=0; fx < numCov.rows(); ++fx) {
		if (numCov(fx) == 1) candidate.push_back(fx);
	}
	if (candidate.size() > 1) {
		std::vector<bool> mask(param.cols());
		for (int cx=candidate.size() - 1; cx >= 0; --cx) {
			std::vector<bool> loading(param.cols());
			for (int ix=0; ix < param.cols(); ++ix) {
				loading[ix] = param(candidate[cx], ix) != 0;
			}
			std::vector<bool> overlap(loading.size());
			std::transform(loading.begin(), loading.end(),
				       mask.begin(), overlap.begin(),
				       std::logical_and<bool>());
			if (std::find(overlap.begin(), overlap.end(), true) == overlap.end()) {
				std::transform(loading.begin(), loading.end(),
					       mask.begin(), mask.begin(),
					       std::logical_or<bool>());
				orthogonal.push_back(candidate[cx]);
			}
		}
	}
	std::reverse(orthogonal.begin(), orthogonal.end());

	if (orthogonal.size() == 1) orthogonal.clear();
	if (orthogonal.size() && orthogonal[0] != mean.cols() - int(orthogonal.size())) {
		Rf_error("Independent specific factors must be given after general dense factors");
	}

	numSpecific = orthogonal.size();

	if (numSpecific) {
		Sgroup.assign(param.cols(), 0);
		for (int ix=0; ix < param.cols(); ix++) {
			for (int dx=orthogonal[0]; dx < abilities; ++dx) {
				if (param(dx, ix) != 0) {
					Sgroup[ix] = dx - orthogonal[0];
					continue;
				}
			}
		}
	}
}

void ifaGroup::setMinItemsPerScore(int mips)
{
	if (numItems() && mips > numItems()) {
		Rf_error("minItemsPerScore (=%d) cannot be larger than the number of items (=%d)",
			 mips, numItems());
	}
	minItemsPerScore = mips;
}

void ifaGroup::buildRowSkip()
{
	rowSkip.assign(rowMap.size(), false);

	if (maxAbilities == 0) return;

	// Rows with no information about an ability will obtain the
	// prior distribution as an ability estimate. This will
	// throw off multigroup latent distribution estimates.
	for (size_t rx=0; rx < rowMap.size(); rx++) {
		bool hasNA = false;
		std::vector<int> contribution(maxAbilities);
		for (int ix=0; ix < numItems(); ix++) {
			int pick = dataColumn(ix)[ rowMap[rx] ];
			if (pick == NA_INTEGER) {
				hasNA = true;
				continue;
			}
			const double *ispec = spec[ix];
			int dims = ispec[RPF_ISpecDims];
			double *iparam = getItemParam(ix);
			for (int dx=0; dx < dims; dx++) {
				// assume factor loadings are the first item parameters
				if (iparam[dx] == 0) continue;
				contribution[dx] += 1;
			}
		}
		if (!hasNA) continue;
		if (minItemsPerScore == NA_INTEGER) {
			Rf_error("You have missing data. You must set minItemsPerScore");
		}
		for (int ax=0; ax < maxAbilities; ++ax) {
			if (contribution[ax] < minItemsPerScore) {
				// We could compute the other scores, but estimation of the
				// latent distribution is in the hot code path. We can reconsider
				// this choice when we try generating scores instead of the
				// score distribution.
				rowSkip[rx] = true;
			}
		}
	}
}

void ba81NormalQuad::layer::allocBuffers(int numThreads, bool wantSummary)
{
	Qweight.resize(weightTableSize, numThreads);
	if (wantSummary) Dweight.resize(weightTableSize, numThreads);

	if (!numSpecific) return;

	thrEi.resize(totalPrimaryPoints, numThreads);
	thrEis.resize(totalPrimaryPoints * numSpecific, numThreads);
}

void ba81NormalQuad::allocBuffers(int numThreads, bool wantSummary)
{
	for (size_t lx=0; lx < layers.size(); ++lx) {
		layers[lx].allocBuffers(numThreads, wantSummary);
	}
}

void ba81NormalQuad::layer::releaseBuffers()
{
	Qweight.resize(0,0);
	Dweight.resize(0,0);
	thrEi.resize(0,0);
	thrEis.resize(0,0);
}

void ba81NormalQuad::releaseBuffers()
{
	for (size_t lx=0; lx < layers.size(); ++lx) {
		layers[lx].releaseBuffers();
	}
}

void ifaGroup::setGridFineness(double width, int points)
{
	if (std::isfinite(width)) qwidth = width;
	if (points != NA_INTEGER) qpoints = points;
}
