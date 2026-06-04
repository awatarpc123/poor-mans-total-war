from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.units import cm
from reportlab.lib.enums import TA_JUSTIFY, TA_LEFT
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, ListFlowable, ListItem
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont

pdfmetrics.registerFont(TTFont('Calibri', 'C:/Windows/Fonts/calibri.ttf'))
pdfmetrics.registerFont(TTFont('Calibri-Bold', 'C:/Windows/Fonts/calibrib.ttf'))
pdfmetrics.registerFont(TTFont('Calibri-Italic', 'C:/Windows/Fonts/calibrii.ttf'))
pdfmetrics.registerFontFamily('Calibri', normal='Calibri', bold='Calibri-Bold', italic='Calibri-Italic')

doc = SimpleDocTemplate(
    "wodomierz.pdf",
    pagesize=A4,
    leftMargin=2.2*cm, rightMargin=2.2*cm,
    topMargin=2*cm, bottomMargin=2*cm,
    title="Naliczanie poboru wody bez armatury",
)

body = ParagraphStyle('body', fontName='Calibri', fontSize=11, leading=15,
                     alignment=TA_JUSTIFY, spaceAfter=10)
h2 = ParagraphStyle('h2', fontName='Calibri-Bold', fontSize=14, leading=18,
                    spaceBefore=14, spaceAfter=8, textColor='#1a1a1a')
list_item = ParagraphStyle('li', fontName='Calibri', fontSize=11, leading=15,
                           alignment=TA_JUSTIFY, spaceAfter=6)

story = []

story.append(Paragraph(
    'Naliczanie poboru wody w nowym mieszkaniu bez armatury wynika zazwyczaj z '
    '<b>wycieku w instalacji</b> (np. nieszczelne zaślepki, pęknięty pion) lub '
    '<b>cofania się wody</b> w rurach przy zmianach ciśnienia (tzw. praca pulsująca). '
    'Czasami winny jest sam <b>uszkodzony licznik</b>.',
    body))

story.append(Paragraph(
    'Najczęstsze przyczyny i kroki, które należy podjąć w pierwszej kolejności:',
    body))

numbered = [
    Paragraph('<b>Nieszczelności za wodomierzem:</b> W mieszkaniu w stanie surowym '
              'wyjścia instalacji są zazwyczaj zakończone gwintami z korkami. Jeśli są '
              'one luźno wkręcone, skorodowane lub brakuje uszczelek, woda może stale '
              'ciec np. do posadzki lub w ścianę.', list_item),
    Paragraph('<b>Cofanie się wody w instalacji (pulsowanie):</b> Jeśli w głównym '
              'pionie budynku występują wahania ciśnienia, woda w rurach Twojego '
              'mieszkania może poruszać się „w tę i z powrotem". Każdy ruch wirnika '
              'w jedną stronę nabija litry na liczniku, co przy braku zaworu '
              'zwrotnego sumuje sztuczne zużycie.', list_item),
    Paragraph('<b>Zapowietrzenie instalacji:</b> Bąble powietrza w instalacji '
              'wodociągowej powodują skoki ciśnienia, które obracają wirnik '
              'wodomierza, mimo że woda faktycznie nie jest pobierana.', list_item),
    Paragraph('<b>Wada fabryczna lub uszkodzenie licznika:</b> Zablokowany lub '
              'wyeksploatowany mechanizm zębaty urządzenia może samoczynnie naliczać '
              'przepływ.', list_item),
    Paragraph('<b>Zjawisko „cofki" i wahania ciśnienia:</b> Bardzo częsty problem w '
              'nowych blokach, gdzie trwają intensywne prace i ciśnienie w pionach '
              'drastycznie się zmienia.<br/>'
              '<b>Działanie:</b> Przy otwartym zaworze głównym obserwuj małe '
              'czarne/czerwone kółko zębate („gwiazdkę") na tarczy wodomierza.<br/>'
              '<b>Diagnoza:</b> Jeśli gwiazdka kręci się do przodu, a potem lekko '
              'cofa (lub tylko „drży" w miejscu, powoli nabijając litry), masz do '
              'czynienia z wahaniami ciśnienia. Woda z Twoich rur jest wtłaczana i '
              'wysysana z powrotem do pionu, gdy sąsiedzi odkręcają wodę, a wodomierz '
              'interpretuje to jako zużycie.<br/>'
              '<b>Rozwiązanie:</b> Oznacza to brak <b>zaworu zwrotnego</b> '
              '(antyskażeniowego) za wodomierzem lub jego uszkodzenie. Jego montaż '
              'rozwiązuje ten problem natychmiast.', list_item),
    Paragraph('<b>„Czynnik ludzki" i błędy ewidencyjne:</b><br/>'
              '<b>Kradzież wody:</b> Upewnij się, że do Twojego zaworu/kranu na '
              'korytarzu (jeśli szafka jest na zewnątrz) nie podpinają się ekipy '
              'wykończeniowe z innych mieszkań.<br/>'
              '<b>Zły licznik:</b> To plaga u deweloperów. Sprawdź numer seryjny '
              'wodomierza z protokołem zdawczo-odbiorczym. Bardzo często szafki są '
              'źle opisane i patrzysz na licznik sąsiada, który akurat kładzie '
              'gładzie i zużywa wodę.', list_item),
    Paragraph('<b>Niewidoczny wyciek pod wylewką (najgorszy scenariusz):</b> Brak '
              'wody na ścianach <i>nie wyklucza</i> wycieku. Rury (np. typu PEX) '
              'idą w peszlach pod warstwą styropianu i wylewki betonowej. Jeśli '
              'instalator uszkodził rurę lub źle zgrzał złączkę w podłodze, woda '
              'może miesiącami gromadzić się pod posadzką, zanim wyjdzie na wierzch '
              'u Ciebie lub zaleje sufit sąsiada z dołu.<br/>'
              '<b>Jak sprawdzić:</b> Otwórz zawór główny na noc. Zrób zdjęcie stanu '
              'licznika wieczorem i rano (kiedy nikt w pionie nie używa wody, więc '
              'wykluczamy skoki ciśnienia). Jeśli rano stan jest wyższy choćby o '
              'litr, a zawór był otwarty — to twardy dowód na nieszczelność '
              'instalacji wewnątrz lokalu.', list_item),
]
story.append(ListFlowable(
    [ListItem(p, leftIndent=18, value=i+1) for i, p in enumerate(numbered)],
    bulletType='1', bulletFontName='Calibri-Bold', bulletFontSize=11,
    leftIndent=18))

story.append(Spacer(1, 6))
story.append(Paragraph('Jak to szybko zweryfikować?', h2))

story.append(Paragraph(
    'Najprostszym testem jest zamknięcie głównego zaworu odcinającego tuż <i>za</i> '
    'wodomierzem:', body))

bullets1 = [
    Paragraph('Jeżeli po zakręceniu zaworu licznik przestaje naliczać wodę → usterka '
              'lub wyciek znajduje się w <b>Twojej instalacji wewnętrznej</b> w '
              'mieszkaniu.', list_item),
    Paragraph('Jeżeli licznik nadal bije → problem leży po stronie samego wodomierza '
              'lub <b>instalacji pionu głównego</b> (zarządcy budynku).', list_item),
]
story.append(ListFlowable(
    [ListItem(p, leftIndent=18) for p in bullets1],
    bulletType='bullet', bulletFontName='Calibri', bulletFontSize=11,
    leftIndent=18))

story.append(Spacer(1, 6))
story.append(Paragraph('W celu rozwiązania tego problemu warto:', body))

bullets2 = [
    Paragraph('Zgłosić sytuację do <b>administratora budynku</b> lub <b>dewelopera</b> '
              'celem weryfikacji i wykonania próby ciśnieniowej.', list_item),
    Paragraph('Sprawdzić, czy na instalacji zamontowany jest sprawny <b>zawór '
              'zwrotny</b>.', list_item),
]
story.append(ListFlowable(
    [ListItem(p, leftIndent=18) for p in bullets2],
    bulletType='bullet', bulletFontName='Calibri', bulletFontSize=11,
    leftIndent=18))

story.append(Spacer(1, 8))
story.append(Paragraph('Co powinieneś zrobić teraz', h2))
story.append(Paragraph(
    '<b>Zakręć zawór główny za wodomierzem.</b> Jeśli przy zakręconym zaworze '
    'licznik nagle przestanie nabijać wodę przez <b>kolejne dni</b>, problem w '
    '100% leży po stronie rur fizycznie wchodzących do Twojego mieszkania '
    '(wyciek w podłodze, powietrze w rurach lub podpięty sąsiad). To daje Ci '
    'podstawę do natychmiastowego wezwania <b>dewelopera w ramach rękojmi</b>.',
    body))

story.append(Spacer(1, 10))
story.append(Paragraph(
    'Jeśli chcesz, abym pomógł Ci głębiej przeanalizować ten problem, napisz:', body))

questions = [
    Paragraph('Czy w mieszkaniu są zamontowane jakiekolwiek krany/baterie lub '
              'spłuczki podtynkowe?', list_item),
    Paragraph('Czy licznik bije stale, czy tylko w konkretnych porach dnia?', list_item),
    Paragraph('Czy instalacja była już testowana ciśnieniowo?', list_item),
]
story.append(ListFlowable(
    [ListItem(p, leftIndent=18) for p in questions],
    bulletType='bullet', bulletFontName='Calibri', bulletFontSize=11,
    leftIndent=18))

doc.build(story)
print("OK")
